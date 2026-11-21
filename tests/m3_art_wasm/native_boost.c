#include <jni.h>
#include <stdint.h>

JNIEXPORT jint JNICALL Java_BenchmarkApp_nativeBoost(JNIEnv* env, jclass cls, jint x,
                                                     jstring tag) {
    const char* text = (*env)->GetStringUTFChars(env, tag, 0);
    if (!text) return -1;
    jint length = 0;
    while (text[length]) ++length;
    (*env)->ReleaseStringUTFChars(env, tag, text);

    jstring reply = (*env)->NewStringUTF(env, "reply");
    jmethodID callback = (*env)->GetStaticMethodID(env, cls, "fromNative",
                                                  "(ILjava/lang/String;)I");
    if (!reply || !callback) return -2;
    const jint result = (*env)->CallStaticIntMethod(env, cls, callback, x, reply);
    if ((*env)->ExceptionCheck(env)) return 0;

    const char* returned = (*env)->GetStringUTFChars(env, reply, 0);
    if (!returned) return -3;
    const jboolean survived = returned[0] == 'r' && returned[1] == 'e' &&
        returned[2] == 'p' && returned[3] == 'l' && returned[4] == 'y' && returned[5] == 0;
    (*env)->ReleaseStringUTFChars(env, reply, returned);
    return survived ? (jint)((uint32_t)result + (uint32_t)length) : -4;
}

static jint native_echo(JNIEnv* env, jclass cls, jint x) {
    (void)env;
    (void)cls;
    return (jint)((uint32_t)x * 2);
}

JNIEXPORT jint JNICALL wa_art_register_natives(JNIEnv* env) {
    jclass cls = (*env)->FindClass(env, "BenchmarkApp");
    if (!cls) return JNI_ERR;
    static const JNINativeMethod methods[] = {
        {"nativeBoost", "(ILjava/lang/String;)I", (void*)Java_BenchmarkApp_nativeBoost},
        {"nativeEcho", "(I)I", (void*)native_echo},
    };
    return (*env)->RegisterNatives(env, cls, methods, sizeof(methods) / sizeof(methods[0]));
}
