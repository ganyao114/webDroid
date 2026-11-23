#include "hybrid_math.h"

#include <android/log.h>
#include <dlfcn.h>
#include <jni.h>
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENV(env, name, ...) (*(env))->name((env), ##__VA_ARGS__)

_Static_assert(offsetof(struct JNIInvokeInterface, GetEnv) == 6 * sizeof(void*), "JavaVM slot");
_Static_assert(offsetof(struct JNINativeInterface, GetObjectRefType) == 232 * sizeof(void*), "JNIEnv slot");
_Static_assert(sizeof(jvalue) == 8, "jvalue size");

static jclass bridge;
static int loads;
static uint8_t native_bytes[8] = {0x21};

static jstring native_abi(JNIEnv* env, jclass unused) {
#if defined(__aarch64__)
    return ENV(env, NewStringUTF, "arm64-v8a");
#else
    return ENV(env, NewStringUTF, "armeabi-v7a");
#endif
}

static jint load_count(JNIEnv* env, jclass unused) {
    return loads;
}

static jlong checksum(JNIEnv* env, jclass unused, jint seed) {
    return (jlong)hybrid_mix_checksum(seed);
}

static jdouble score(JNIEnv* env, jclass unused, jdouble x, jdouble y, jdouble z) {
    return hybrid_dot_f64(x, y, z);
}

static jfloat scale(JNIEnv* env, jclass unused, jfloat input, jint by) {
    return input * (jfloat)by;
}

static jobject native_buffer(JNIEnv* env, jclass unused) {
    return ENV(env, NewDirectByteBuffer, native_bytes, (jlong)sizeof(native_bytes));
}

static jint read_native_buffer(JNIEnv* env, jclass unused) {
    return native_bytes[1];
}

static void require_positive(JNIEnv* env, jclass unused, jint input) {
    if (input < 0) {
        jclass type = ENV(env, FindClass, "java/lang/IllegalArgumentException");
        if (type) ENV(env, ThrowNew, type, "Input must be positive");
    }
}

static jint run(JNIEnv* env, jclass unused, jobject activity, jint seed, jbyteArray scratch, jobject buffer) {
    jbyte* elements = ENV(env, GetByteArrayElements, scratch, NULL);
    if (!elements) return -1;
    elements[0] = 6;
    ENV(env, ReleaseByteArrayElements, scratch, elements, JNI_COMMIT);
    elements[1] = 8;
    ENV(env, ReleaseByteArrayElements, scratch, elements, JNI_ABORT);
    elements = ENV(env, GetPrimitiveArrayCritical, scratch, NULL);
    if (!elements) return -2;
    elements[2] = 9;
    ENV(env, ReleasePrimitiveArrayCritical, scratch, elements, 0);

    uint8_t* memory = ENV(env, GetDirectBufferAddress, buffer);
    if (!memory || ENV(env, GetDirectBufferCapacity, buffer) != 7) return -3;
    memory[0] = 0x59;
    memory[1] = 0x80 + (uint8_t)seed;

    jint bins[4];
    hybrid_fill_histogram(seed, bins);
    jintArray result = ENV(env, NewIntArray, 4);
    ENV(env, SetIntArrayRegion, result, 0, 4, bins);
    jclass type = ENV(env, GetObjectClass, activity);
    jfieldID visits = ENV(env, GetFieldID, type, "nativeVisits", "I");
    jfieldID instance_score = ENV(env, GetFieldID, type, "nativeScore", "D");
    jfieldID instance_checksum = ENV(env, GetFieldID, type, "nativeChecksum", "J");
    jmethodID report = ENV(env, GetMethodID, type, "onNativeReport", "(Ljava/lang/String;JD[I)V");
    jmethodID bump = ENV(env, GetStaticMethodID, bridge, "staticBump", "(I)I");
    jfieldID last_long = ENV(env, GetStaticFieldID, bridge, "lastLong", "J");
    jfieldID last_float = ENV(env, GetStaticFieldID, bridge, "lastFloat", "F");
    jfieldID last_double = ENV(env, GetStaticFieldID, bridge, "lastDouble", "D");
    jfieldID last_byte = ENV(env, GetStaticFieldID, bridge, "lastByte", "B");
    if (ENV(env, ExceptionCheck)) return -4;

    if (ENV(env, PushLocalFrame, 4) != JNI_OK) return -5;
    jstring title = ENV(env, NewStringUTF, "Native report");
    title = (jstring)ENV(env, PopLocalFrame, title);
    if (ENV(env, GetObjectRefType, title) != JNILocalRefType) return -6;
    if (ENV(env, MonitorEnter, activity) != JNI_OK) return -7;
    const jint previous_visits = ENV(env, GetIntField, activity, visits);
    ENV(env, SetIntField, activity, visits, previous_visits + 1);
    const jlong sum = (jlong)hybrid_mix_checksum(seed);
    const jdouble dot = hybrid_dot_f64(2.75 + seed, 1.25, 2.0);
    ENV(env, CallVoidMethod, activity, report, title, sum, dot, result);
    ENV(env, MonitorExit, activity);
    if (ENV(env, ExceptionCheck)) return -8;
    if (ENV(env, GetDoubleField, activity, instance_score) != dot ||
        ENV(env, GetLongField, activity, instance_checksum) != sum) return -9;
    ENV(env, SetStaticLongField, bridge, last_long, sum);
    ENV(env, SetStaticFloatField, bridge, last_float, 1.25f);
    ENV(env, SetStaticDoubleField, bridge, last_double, dot);
    ENV(env, SetStaticByteField, bridge, last_byte, (jbyte)-7);
    if (ENV(env, GetStaticFloatField, bridge, last_float) != 1.25f) return -10;

    return ENV(env, CallStaticIntMethod, bridge, bump, seed);
}

static jdouble wide(JNIEnv* env, jclass unused, jint a, jint b, jint c, jint d,
                    jint e, jint f, jint g, jint h, jint i, jlong value,
                    jdouble x1, jdouble x2, jdouble x3, jdouble x4, jdouble x5,
                    jdouble x6, jdouble x7, jdouble x8, jdouble x9, jdouble x10, jfloat tail) {
    const int total = a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h + 9 * i;
    return total + (int)(value >> 32) + x1 + 2 * x2 + 3 * x3 + 4 * x4 + 5 * x5
        + 6 * x6 + 7 * x7 + 8 * x8 + 9 * x9 + 10 * x10 + tail;
}

static jdouble call_static_v(JNIEnv* env, jmethodID id, ...) {
    va_list args;
    va_start(args, id);
    const jdouble value = ENV(env, CallStaticDoubleMethodV, bridge, id, args);
    va_end(args);
    return value;
}

static int print_v(char* dst, size_t size, const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int length = vsnprintf(dst, size, format, args);
    va_end(args);
    return length;
}

#define WIDE_ARGUMENTS 1, 2, 3, 4, 5, 6, 7, 8, 9, (jlong)UINT64_C(0x700000002), \
    0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 4.5, 5.0, 0.25

static jboolean verify_abi(JNIEnv* env, jclass unused) {
    jmethodID mix = ENV(env, GetStaticMethodID, bridge, "sumMixed", "(IIIIIIIIIJDDDDDDDDDDF)D");
    if (!mix) return JNI_FALSE;
    const jdouble direct = ENV(env, CallStaticDoubleMethod, bridge, mix, WIDE_ARGUMENTS);
    const jdouble v = call_static_v(env, mix, WIDE_ARGUMENTS);
    const jvalue args[] = {{.i=1}, {.i=2}, {.i=3}, {.i=4}, {.i=5}, {.i=6}, {.i=7}, {.i=8}, {.i=9},
        {.j=UINT64_C(0x700000002)}, {.d=0.5}, {.d=1}, {.d=1.5}, {.d=2}, {.d=2.5}, {.d=3},
        {.d=3.5}, {.d=4}, {.d=4.5}, {.d=5}, {.f=0.25f}};
    const jdouble a = ENV(env, CallStaticDoubleMethodA, bridge, mix, args);
    if (direct != 484.75 || v != direct || a != direct) return JNI_FALSE;

    const jchar unicode[] = {0x4a, 0, 0xe9};
    jstring value = ENV(env, NewString, unicode, 3);
    if (!value || ENV(env, GetStringUTFLength, value) != 5) return JNI_FALSE;
    char utf8[6] = {0};
    ENV(env, GetStringUTFRegion, value, 0, 3, utf8);
    if (memcmp(utf8, "J\xc0\x80\xc3\xa9", 5) != 0) return JNI_FALSE;
    jweak weak = ENV(env, NewWeakGlobalRef, value);
    if (ENV(env, GetObjectRefType, weak) != JNIWeakGlobalRefType ||
        !ENV(env, IsSameObject, value, weak)) return JNI_FALSE;
    ENV(env, DeleteWeakGlobalRef, weak);
    ENV(env, DeleteLocalRef, value);

    char text[40];
    if (print_v(text, sizeof(text), "%s:%lld:%.2f", "jni", (long long)hybrid_mix_checksum(0), 3.25) != 19 ||
        strcmp(text, "jni:4294967297:3.25") != 0) return JNI_FALSE;

    void* library = dlopen("libhybrid_math.so", RTLD_NOW);
    if (!library) return JNI_FALSE;
    int (*constructors)(void) = (int (*)(void))dlsym(library, "hybrid_constructor_count");
    const int count = constructors ? constructors() : -1;
    void* missing = dlsym(library, "hybrid_symbol_absent");
    const char* error = dlerror();
    const int reported = error && strstr(error, "hybrid_symbol_absent");
    const char* consumed = dlerror();
    return count == 1 && !missing && reported && !consumed && dlclose(library) == 0;
}

JNIEXPORT jstring JNICALL
Java_com_wasmandroid_hybrid_HybridBridge_dynamicBanner(JNIEnv* env, jclass unused, jstring label, jint seed) {
    const char* prefix = ENV(env, GetStringUTFChars, label, NULL);
    if (!prefix) return NULL;
    char text[64];
    snprintf(text, sizeof(text), "%s %llu", prefix, (unsigned long long)hybrid_mix_checksum(seed));
    ENV(env, ReleaseStringUTFChars, label, prefix);
    return ENV(env, NewStringUTF, text);
}

JNIEXPORT jint JNICALL
Java_com_wasmandroid_hybrid_HybridBridge_decorated_1name___3I(JNIEnv* env, jclass unused, jintArray array) {
    jint values[2];
    if (ENV(env, GetArrayLength, array) != 2) return -1;
    ENV(env, GetIntArrayRegion, array, 0, 2, values);
    return values[0] + values[1];
}

JNIEXPORT jint JNICALL
Java_com_wasmandroid_hybrid_HybridBridge_decorated_1name__Ljava_lang_String_2(
        JNIEnv* env, jclass unused, jstring value) {
    return ENV(env, GetStringLength, value);
}

JNIEXPORT jint JNICALL
Java_com_wasmandroid_hybrid_HybridBridge_m_000e9tier(JNIEnv* env, jclass unused) {
    return 17;
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* unused) {
    JNIEnv* env = NULL;
    if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK || !env) return JNI_ERR;
    JavaVM* same_vm = NULL;
    if (ENV(env, GetJavaVM, &same_vm) != JNI_OK || same_vm != vm) return JNI_ERR;
    jclass local = ENV(env, FindClass, "com/wasmandroid/hybrid/HybridBridge");
    if (!local || ENV(env, GetVersion) != JNI_VERSION_1_6 || hybrid_constructor_count() != 1) return JNI_ERR;
    bridge = (jclass)ENV(env, NewGlobalRef, local);
    if (!bridge || ENV(env, GetObjectRefType, bridge) != JNIGlobalRefType) return JNI_ERR;

    const JNINativeMethod methods[] = {
        {"abi", "()Ljava/lang/String;", native_abi},
        {"loadCount", "()I", load_count},
        {"checksum", "(I)J", checksum},
        {"score", "(DDD)D", score},
        {"scale", "(FI)F", scale},
        {"run", "(Lcom/wasmandroid/hybrid/HybridActivity;I[BLjava/nio/ByteBuffer;)I", run},
        {"nativeBuffer", "()Ljava/nio/ByteBuffer;", native_buffer},
        {"readNativeBuffer", "()I", read_native_buffer},
        {"requirePositive", "(I)V", require_positive},
        {"verifyAbi", "()Z", verify_abi},
        {"wide", "(IIIIIIIIIJDDDDDDDDDDF)D", wide},
    };
    if (ENV(env, RegisterNatives, bridge, methods, sizeof(methods) / sizeof(methods[0])) != JNI_OK) return JNI_ERR;
    ++loads;
    __android_log_print(ANDROID_LOG_INFO, "HybridJNI", "JNI 1.6, constructor count %d", hybrid_constructor_count());
    return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM* vm, void* unused) {
    JNIEnv* env = NULL;
    if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) == JNI_OK && env) ENV(env, DeleteGlobalRef, bridge);
}
