#include <jni.h>
#include <stdint.h>

static jint increment(JNIEnv* env, jobject object, jint value) {
    (void)env;
    (void)object;
    return value + 1;
}

static void finish(JNIEnv* env, jobject object) {
    (void)env;
    (void)object;
}

JNIEXPORT jint JNICALL wa_jni_probe(JNIEnv* env) {
    const jclass clazz = (*env)->FindClass(env, "com/example/Contract");
    if (!clazz) {
        return -1;
    }
    const jstring string = (*env)->NewStringUTF(env, "A\xc0\x80Z");
    if (!string) {
        return -2;
    }
    static const JNINativeMethod methods[] = {
        {"increment", "(I)I", (void*)increment},
        {"finish", "()V", (void*)finish},
    };
    if ((*env)->RegisterNatives(env, clazz, methods, 2) != JNI_OK) {
        return -3;
    }
    const jvalue args[] = {{.i = -123}, {.l = string}};
    return (*env)->CallIntMethodA(env, string, (jmethodID)(uintptr_t)UINT64_C(0x100000012), args);
}
