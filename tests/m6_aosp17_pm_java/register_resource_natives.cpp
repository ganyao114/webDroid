#include <jni.h>

namespace android {
int register_android_content_res_ApkAssets(JNIEnv* env);
int register_android_content_StringBlock(JNIEnv* env);
int register_android_content_XmlBlock(JNIEnv* env);
}  // namespace android

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK ||
      android::register_android_content_StringBlock(env) != JNI_OK ||
      android::register_android_content_XmlBlock(env) != JNI_OK ||
      android::register_android_content_res_ApkAssets(env) != JNI_OK) {
    return JNI_ERR;
  }
  return JNI_VERSION_1_6;
}
