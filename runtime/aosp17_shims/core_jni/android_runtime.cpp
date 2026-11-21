#include <android_runtime/AndroidRuntime.h>

namespace android {

// Only the loader's callbacks into Java need this part of libandroid_runtime.
JNIEnv* AndroidRuntime::getJNIEnv() {
    JavaVM* vm = nullptr;
    jsize count = 0;
    if (JNI_GetCreatedJavaVMs(&vm, 1, &count) != JNI_OK || count == 0) {
        return nullptr;
    }

    void* env = nullptr;
    return vm->GetEnv(&env, JNI_VERSION_1_4) == JNI_OK ? static_cast<JNIEnv*>(env) : nullptr;
}

} // namespace android
