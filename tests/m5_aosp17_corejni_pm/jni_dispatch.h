#pragma once

#include <jni.h>
#include <utils/String8.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct _jfieldID {
    jclass owner;
    std::string name;
    std::string signature;
};

struct _jmethodID {
    jclass owner;
    std::string name;
    std::string signature;
};

namespace wa::jni_test {

[[noreturn]] inline void Fail(std::string_view message,
                              std::source_location location = std::source_location::current()) {
    std::fprintf(stderr, "%s:%u: %.*s\n", location.file_name(), location.line(),
                 static_cast<int>(message.size()), message.data());
    std::exit(EXIT_FAILURE);
}

inline void Check(bool ok, std::string_view message,
                  std::source_location location = std::source_location::current()) {
    if (!ok) {
        Fail(message, location);
    }
}

struct ClassRef : _jclass {
    explicit ClassRef(const char* value) : name(value) {}

    std::string name;
    std::map<std::pair<std::string, std::string>, void*> natives;
};

struct StringRef : _jstring {
    explicit StringRef(const char* value) : utf8(value) {}
    std::string utf8;
};

template <typename Ref, typename T> struct ArrayRef : Ref {
    ArrayRef(jsize length, T initial) : values(length, initial) {}
    std::vector<T> values;
};

using ObjectArray = ArrayRef<_jobjectArray, jobject>;
using ByteArray = ArrayRef<_jbyteArray, jbyte>;
using IntArray = ArrayRef<_jintArray, jint>;

struct ApkAssetsRef : _jobject {
    explicit ApkAssetsRef(jlong value) : native_ptr(value) {}
    jlong native_ptr;
};

struct FileDescriptorRef : _jobject {
    explicit FileDescriptorRef(jint value) : descriptor(value) {}
    jint descriptor;
};

struct TypedValueRef : _jobject {
    jint type = 0;
    jint cookie = 0;
    jint data = 0;
    jint id = 0;
};

struct ThrowableRef : _jthrowable {
    ThrowableRef(jclass type, const char* detail) : clazz(type), message(detail) {}
    jclass clazz;
    std::string message;
};

// A process-local JNI environment for the fixture's strings, arrays, and Java fields.
// The upstream registration routines populate the native tables through RegisterNatives.
class JniDispatch : public JNIEnv {
  public:
    JniDispatch() : JNIEnv{&kFunctions} {}
    JniDispatch(const JniDispatch&) = delete;
    JniDispatch& operator=(const JniDispatch&) = delete;

    template <typename T, typename... Args> T* MakeObject(const char* class_name, Args&&... args) {
        T* ref = Make<T>(std::forward<Args>(args)...);
        object_classes_.emplace(ref, FindClass(class_name));
        return ref;
    }

    static const std::string& Utf8(jstring str) {
        Check(str != nullptr, "Expected a Java string");
        return static_cast<StringRef*>(str)->utf8;
    }

    void CheckNoException() const {
        if (exception_) {
            Fail(static_cast<ClassRef*>(exception_->clazz)->name + ": " + exception_->message);
        }
    }

    void ExpectException(std::string_view type, std::string_view message) {
        Check(ExceptionCheck(), "Expected a pending Java exception");
        Check(static_cast<ClassRef*>(exception_->clazz)->name == type &&
                  exception_->message == message,
              "Unexpected Java exception class or message");
        ExceptionClear();
    }

  private:
    static JniDispatch& From(JNIEnv* env) { return *static_cast<JniDispatch*>(env); }

    template <typename T, typename... Args> T* Make(Args&&... args) {
        auto ref = std::make_shared<T>(std::forward<Args>(args)...);
        T* ptr = ref.get();
        refs_.push_back(std::move(ref));
        return ptr;
    }

    template <typename Array, typename T> Array* MakeArray(jsize length, T value) {
        Check(length >= 0, "Negative JNI array length");
        Array* array = Make<Array>(length, value);
        array_lengths_.emplace(array, length);
        return array;
    }

    static JNINativeInterface MakeFunctions() {
        JNINativeInterface table{};
        table.FindClass = [](JNIEnv* env, const char* name) -> jclass {
            return &From(env).classes_.try_emplace(name, name).first->second;
        };
        table.GetFieldID = [](JNIEnv* env, jclass clazz, const char* name, const char* signature) {
            return &From(env).fields_.emplace_back(clazz, name, signature);
        };
        table.GetMethodID = [](JNIEnv* env, jclass clazz, const char* name, const char* signature) {
            return &From(env).methods_.emplace_back(clazz, name, signature);
        };
        table.GetStaticMethodID = table.GetMethodID;
        table.IsInstanceOf = [](JNIEnv* env, jobject obj, jclass clazz) -> jboolean {
            const auto& classes = From(env).object_classes_;
            const auto type = classes.find(obj);
            return obj == nullptr || (type != classes.end() && type->second == clazz);
        };
        table.RegisterNatives = [](JNIEnv*, jclass clazz, const JNINativeMethod* methods,
                                   jint count) {
            auto& natives = static_cast<ClassRef*>(clazz)->natives;
            for (jint i = 0; i < count; ++i) {
                const auto& method = methods[i];
                const auto key = std::pair(method.name, method.signature);
                Check(method.fnPtr && natives.emplace(key, method.fnPtr).second,
                      "Invalid or duplicate native registration");
            }
            return JNI_OK;
        };
        // Keep object references alive for the lifetime of this single test environment.
        table.NewGlobalRef = [](JNIEnv*, jobject ref) { return ref; };
        table.DeleteLocalRef = [](JNIEnv*, jobject) {};
        table.NewStringUTF = [](JNIEnv* env, const char* value) -> jstring {
            return value ? From(env).Make<StringRef>(value) : nullptr;
        };
        table.NewString = [](JNIEnv* env, const jchar* value, jsize length) -> jstring {
            const std::u16string utf16(value, value + length);
            return From(env).Make<StringRef>(android::String8(utf16.data(), utf16.size()).c_str());
        };
        table.GetStringUTFChars = [](JNIEnv*, jstring str, jboolean* copy) {
            if (copy) {
                *copy = JNI_FALSE;
            }
            return Utf8(str).c_str();
        };
        table.ReleaseStringUTFChars = [](JNIEnv*, jstring, const char*) {};
        table.NewObjectArray = [](JNIEnv* env, jsize len, jclass, jobject initial) -> jobjectArray {
            return From(env).MakeArray<ObjectArray>(len, initial);
        };
        table.GetObjectArrayElement = [](JNIEnv*, jobjectArray array, jsize index) {
            return static_cast<ObjectArray*>(array)->values.at(index);
        };
        table.GetArrayLength = [](JNIEnv* env, jarray array) {
            return From(env).array_lengths_.at(array);
        };
        table.NewByteArray = [](JNIEnv* env, jsize len) -> jbyteArray {
            return From(env).MakeArray<ByteArray>(len, jbyte{0});
        };
        table.GetByteArrayElements = [](JNIEnv*, jbyteArray array, jboolean* copy) {
            if (copy) {
                *copy = JNI_FALSE;
            }
            return static_cast<ByteArray*>(array)->values.data();
        };
        table.ReleaseByteArrayElements = [](JNIEnv*, jbyteArray, jbyte*, jint) {};
        table.NewIntArray = [](JNIEnv* env, jsize len) -> jintArray {
            return From(env).MakeArray<IntArray>(len, jint{0});
        };
        table.SetIntArrayRegion = [](JNIEnv*, jintArray array, jsize start, jsize len,
                                     const jint* values) {
            auto& dest = static_cast<IntArray*>(array)->values;
            Check(start >= 0 && len >= 0 && static_cast<size_t>(start) + len <= dest.size(),
                  "Invalid JNI int array region");
            std::copy_n(values, len, dest.begin() + start);
        };
        table.GetLongField = [](JNIEnv* env, jobject obj, jfieldID field) {
            Check(obj && env->IsInstanceOf(obj, field->owner) &&
                      static_cast<ClassRef*>(field->owner)->name ==
                          "android/content/res/ApkAssets" &&
                      field->name == "mNativePtr" && field->signature == "J",
                  "Expected ApkAssets.mNativePtr");
            return static_cast<ApkAssetsRef*>(obj)->native_ptr;
        };
        table.GetIntField = [](JNIEnv* env, jobject obj, jfieldID field) {
            Check(obj && env->IsInstanceOf(obj, field->owner) &&
                      static_cast<ClassRef*>(field->owner)->name == "java/io/FileDescriptor" &&
                      field->name == "descriptor" && field->signature == "I",
                  "Expected FileDescriptor.descriptor");
            return static_cast<FileDescriptorRef*>(obj)->descriptor;
        };
        table.CallNonvirtualVoidMethodV = [](JNIEnv* env, jobject obj, jclass clazz,
                                             jmethodID method, va_list args) {
            Check(obj && env->IsInstanceOf(obj, clazz) &&
                      static_cast<ClassRef*>(clazz)->name == "android/util/TypedValue" &&
                      method->owner == clazz && method->name == "setFields" &&
                      method->signature == "(IIIIIIZ)V",
                  "Expected TypedValue.setFields");
            auto& value = *static_cast<TypedValueRef*>(obj);
            value.type = va_arg(args, jint);
            value.cookie = va_arg(args, jint);
            value.data = static_cast<jint>(va_arg(args, uint32_t));
            value.id = static_cast<jint>(va_arg(args, uint32_t));
        };
        table.NewObjectV = [](JNIEnv* env, jclass clazz, jmethodID method,
                              va_list args) -> jobject {
            Check(method->owner == clazz && method->name == "<init>" &&
                      method->signature == "(Ljava/lang/String;)V",
                  "Expected an exception constructor with a message");
            const jstring message = va_arg(args, jstring);
            return From(env).Make<ThrowableRef>(clazz, message ? Utf8(message).c_str() : "");
        };
        table.Throw = [](JNIEnv* env, jthrowable exception) {
            From(env).exception_ = static_cast<ThrowableRef*>(exception);
            return JNI_OK;
        };
        table.ExceptionOccurred = [](JNIEnv* env) -> jthrowable { return From(env).exception_; };
        table.ExceptionCheck = [](JNIEnv* env) -> jboolean {
            return From(env).exception_ != nullptr;
        };
        table.ExceptionClear = [](JNIEnv* env) { From(env).exception_ = nullptr; };
        return table;
    }

    static const JNINativeInterface kFunctions;
    std::map<std::string, ClassRef> classes_;
    std::deque<_jfieldID> fields_;
    std::deque<_jmethodID> methods_;
    std::vector<std::shared_ptr<_jobject>> refs_;
    std::map<jobject, jclass> object_classes_;
    std::map<jarray, jsize> array_lengths_;
    ThrowableRef* exception_ = nullptr;
};

inline const JNINativeInterface JniDispatch::kFunctions = JniDispatch::MakeFunctions();

class NativeClass {
  public:
    NativeClass(JNIEnv& env, const char* name)
        : env_(env), clazz_(static_cast<ClassRef*>(env.FindClass(name))) {
        Check(!clazz_->natives.empty(), "No native methods registered for " + clazz_->name);
    }

    template <typename Result, typename Receiver = jclass, typename... Args>
    Result Call(const char* name, const char* signature, Args... args) const {
        const auto entry = clazz_->natives.find({name, signature});
        Check(entry != clazz_->natives.end(), clazz_->name + "." + name + signature);
        using Method = Result (*)(JNIEnv*, Receiver, Args...);
        return reinterpret_cast<Method>(entry->second)(&env_, clazz_, args...);
    }

  private:
    JNIEnv& env_;
    ClassRef* clazz_;
};

} // namespace wa::jni_test
