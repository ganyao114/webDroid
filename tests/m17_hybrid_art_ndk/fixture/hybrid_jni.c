#include "hybrid_math.h"

#include <android/asset_manager_jni.h>
#include <android/bitmap.h>
#include <android/looper.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <dlfcn.h>
#include <jni.h>
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <unistd.h>

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

static jint patched_load_count(JNIEnv* env, jclass unused) {
    return loads + 7;
}

static jboolean (*original_is_64_bit)(JNIEnv*, jobject);

static jboolean patched_is_64_bit(JNIEnv* env, jobject runtime) {
    return !original_is_64_bit(env, runtime);
}

static jboolean verify_method_id(JNIEnv* env) {
    jmethodID id = ENV(env, GetStaticMethodID, bridge, "loadCount", "()I");
    if (!id) return JNI_FALSE;
    jobject reflected = ENV(env, ToReflectedMethod, bridge, id, JNI_TRUE);
    jclass executable = ENV(env, FindClass, "java/lang/reflect/Executable");
    if (!executable || !reflected || ENV(env, FromReflectedMethod, reflected) != id) return JNI_FALSE;
    jfieldID art_method = ENV(env, GetFieldID, executable, "artMethod", "J");
    if (!art_method || (uintptr_t)ENV(env, GetLongField, reflected, art_method) != (uintptr_t)id) return JNI_FALSE;

    // ArtMethod has 16 bytes of fixed fields before its pointer-sized fields.
    volatile uintptr_t* entry = (volatile uintptr_t*)((uintptr_t)id + 16);
    if (*entry != (uintptr_t)load_count) return JNI_FALSE;
    *entry = (uintptr_t)patched_load_count;
    const jint patched = ENV(env, CallStaticIntMethod, bridge, id);
    *entry = (uintptr_t)load_count;
    if (patched != loads + 7 || ENV(env, CallStaticIntMethod, bridge, id) != loads) return JNI_FALSE;

    jclass vm = ENV(env, FindClass, "dalvik/system/VMRuntime");
    if (!vm) return JNI_FALSE;
    jmethodID get_runtime = ENV(env, GetStaticMethodID, vm, "getRuntime", "()Ldalvik/system/VMRuntime;");
    jmethodID is_64_bit = ENV(env, GetMethodID, vm, "is64Bit", "()Z");
    if (!get_runtime || !is_64_bit) return JNI_FALSE;
    jobject runtime = ENV(env, CallStaticObjectMethod, vm, get_runtime);
    if (!runtime) return JNI_FALSE;
    entry = (volatile uintptr_t*)((uintptr_t)is_64_bit + 16);
    original_is_64_bit = (jboolean (*)(JNIEnv*, jobject))*entry;
    if (!original_is_64_bit) return JNI_FALSE;
    const jboolean original = ENV(env, CallBooleanMethod, runtime, is_64_bit);
    *entry = (uintptr_t)patched_is_64_bit;
    const jboolean inverted = ENV(env, CallBooleanMethod, runtime, is_64_bit);
    *entry = (uintptr_t)original_is_64_bit;
    return inverted == !original && ENV(env, CallBooleanMethod, runtime, is_64_bit) == original;
}

static jboolean verify_non_movable_array(JNIEnv* env) {
    jclass vm = ENV(env, FindClass, "dalvik/system/VMRuntime");
    jclass integer = ENV(env, FindClass, "java/lang/Integer");
    if (!vm || !integer) return JNI_FALSE;
    jfieldID type = ENV(env, GetStaticFieldID, integer, "TYPE", "Ljava/lang/Class;");
    jmethodID get_runtime = ENV(env, GetStaticMethodID, vm, "getRuntime", "()Ldalvik/system/VMRuntime;");
    jmethodID allocate = ENV(env, GetMethodID, vm, "newNonMovableArray", "(Ljava/lang/Class;I)Ljava/lang/Object;");
    jmethodID address_of = ENV(env, GetMethodID, vm, "addressOf", "(Ljava/lang/Object;)J");
    if (!type || !get_runtime || !allocate || !address_of) return JNI_FALSE;
    jobject runtime = ENV(env, CallStaticObjectMethod, vm, get_runtime);
    jobject component = ENV(env, GetStaticObjectField, integer, type);
    if (!runtime || !component) return JNI_FALSE;
    jintArray array = ENV(env, CallObjectMethod, runtime, allocate, component, (jint)2);
    if (!array) return JNI_FALSE;
    jint* storage = (jint*)(uintptr_t)ENV(env, CallLongMethod, runtime, address_of, array);
    if (!storage || ENV(env, GetArrayLength, array) != 2) return JNI_FALSE;
    storage[0] = 123;
    jint value = 0;
    ENV(env, GetIntArrayRegion, array, 0, 1, &value);
    ENV(env, SetIntArrayRegion, array, 1, 1, &value);
    return value == 123 && storage[1] == 123;
}

static jboolean read_elf_header(const char* path) {
    struct stat info;
    if (stat(path, &info) || !S_ISREG(info.st_mode) || info.st_size < 5) return JNI_FALSE;
    FILE* file = fopen(path, "rb");
    if (!file) return JNI_FALSE;
    unsigned char header[5];
    const size_t count = fread(header, 1, sizeof(header), file);
    fclose(file);
    return count == sizeof(header) && memcmp(header, "\177ELF", 4) == 0 &&
           header[4] == (sizeof(void*) == 8 ? 2 : 1);
}

static jboolean verify_library_paths(void) {
#if defined(__aarch64__)
    const char* system = "/system/lib64/libc.so";
    const char* app = "/data/app/com.wasmandroid.hybrid/lib/arm64/libhybrid_math.so";
#else
    const char* system = "/system/lib/libc.so";
    const char* app = "/data/app/com.wasmandroid.hybrid/lib/arm/libhybrid_math.so";
#endif
    if (!read_elf_header(system) || !read_elf_header(app)) return JNI_FALSE;
    void* handle = dlopen(system, RTLD_NOW);
    if (!handle) return JNI_FALSE;
    size_t (*length)(const char*) = (size_t (*)(const char*))dlsym(handle, "strlen");
    const jboolean valid = length && length("libc") == 4;
    return dlclose(handle) == 0 && valid;
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

static jboolean edit_bitmap(JNIEnv* env, jclass unused, jobject bitmap) {
    AndroidBitmapInfo info;
    if (AndroidBitmap_getInfo(env, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS ||
        info.width != 2 || info.height != 1 || info.stride != 8 ||
        info.format != ANDROID_BITMAP_FORMAT_RGBA_8888) return JNI_FALSE;
    void* address;
    if (AndroidBitmap_lockPixels(env, bitmap, &address) != ANDROID_BITMAP_RESULT_SUCCESS) return JNI_FALSE;
    uint32_t* pixels = address;
    const jboolean read_ok = pixels && pixels[1] == UINT32_C(0xff332211);
    if (read_ok) pixels[1] = UINT32_C(0xff445566);
    return AndroidBitmap_unlockPixels(env, bitmap) == ANDROID_BITMAP_RESULT_SUCCESS && read_ok;
}

static jboolean read_asset(JNIEnv* env, jclass unused, jobject manager) {
    static const char expected[] = "Android asset\n";
    AAssetManager* assets = AAssetManager_fromJava(env, manager);
    if (!assets || AAssetManager_open(assets, "absent", AASSET_MODE_BUFFER)) return JNI_FALSE;
    AAsset* asset = AAssetManager_open(assets, "native-probe.txt", AASSET_MODE_BUFFER);
    if (!asset) return JNI_FALSE;
    const void* buffer = AAsset_getBuffer(asset);
    char readback[sizeof(expected) - 1];
    const jboolean matches = AAsset_getLength(asset) == sizeof(expected) - 1 && buffer &&
        memcmp(buffer, expected, sizeof(expected) - 1) == 0 && AAsset_isAllocated(asset) &&
        AAsset_read(asset, readback, sizeof(readback)) == sizeof(readback) &&
        memcmp(readback, expected, sizeof(readback)) == 0 && AAsset_read(asset, readback, 1) == 0;
    AAsset_close(asset);
    return matches;
}

struct looper_lock_probe {
    pthread_mutex_t mutex;
    int acquired;
    int polled;
};

static int lock_from_callback(int fd, int events, void* data) {
    struct looper_lock_probe* probe = data;
    if ((events & ALOOPER_EVENT_INPUT) && pthread_mutex_lock(&probe->mutex) == 0) {
        probe->acquired = 1;
        pthread_mutex_unlock(&probe->mutex);
    }
    return 0;
}

static void* poll_with_caller_lock(void* data) {
    struct looper_lock_probe* probe = data;
    int descriptors[2];
    if (pipe(descriptors) != 0) return NULL;
    ALooper* looper = ALooper_prepare(0);
    const char ready = 1;
    if (looper && ALooper_addFd(looper, descriptors[0], ALOOPER_POLL_CALLBACK,
            ALOOPER_EVENT_INPUT, lock_from_callback, probe) == 1 &&
            write(descriptors[1], &ready, 1) == 1) {
        probe->polled = ALooper_pollOnce(500, NULL, NULL, NULL) == ALOOPER_POLL_CALLBACK;
        ALooper_removeFd(looper, descriptors[0]);
    }
    close(descriptors[0]);
    close(descriptors[1]);
    return NULL;
}

static jboolean run_looper_lock_probe(void) {
    struct looper_lock_probe probe = {.mutex = PTHREAD_MUTEX_INITIALIZER};
    pthread_t thread;
    if (pthread_mutex_lock(&probe.mutex) != 0) return JNI_FALSE;
    const int created = pthread_create(&thread, NULL, poll_with_caller_lock, &probe) == 0;
    if (created) {
        // Cross several guest time slices with the mutex still held.
        volatile unsigned checksum = 0;
        for (unsigned i = 0; i < 20000; ++i) checksum += i;
    }
    pthread_mutex_unlock(&probe.mutex);
    const int joined = created && pthread_join(thread, NULL) == 0;
    pthread_mutex_destroy(&probe.mutex);
    return joined && probe.polled && probe.acquired;
}

static jboolean poll_timer(JNIEnv* env, jclass unused) {
    ALooper* looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    if (!looper || ALooper_forThread() != looper || ALooper_pollOnce(0, NULL, NULL, NULL) != ALOOPER_POLL_TIMEOUT)
        return JNI_FALSE;
    ALooper_wake(looper);
    if (ALooper_pollOnce(0, NULL, NULL, NULL) != ALOOPER_POLL_WAKE) return JNI_FALSE;
    const int timer = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (timer < 0) return JNI_FALSE;
    const struct itimerspec time = {.it_value = {.tv_nsec = 2000000}};
    int ready = -1, events = 0;
    void* data = NULL;
    uint64_t ticks = 0;
    const jboolean ok = ALooper_addFd(looper, timer, 5, ALOOPER_EVENT_INPUT, NULL, &ticks) == 1 &&
        timerfd_settime(timer, 0, &time, NULL) == 0 && ALooper_pollOnce(500, &ready, &events, &data) == 5 &&
        ready == timer && events == ALOOPER_EVENT_INPUT && data == &ticks &&
        read(timer, &ticks, sizeof(ticks)) == sizeof(ticks) && ticks == 1 &&
        ALooper_pollOnce(0, NULL, NULL, NULL) == ALOOPER_POLL_TIMEOUT;
    ALooper_removeFd(looper, timer);
    close(timer);
    return ok && run_looper_lock_probe();
}

static jboolean protect_image_data(JNIEnv* env, jclass unused) {
    static volatile int value = 7;
    const long size = sysconf(_SC_PAGESIZE);
    if (size <= 0) return JNI_FALSE;
    void* page = (void*)((uintptr_t)&value & ~((uintptr_t)size - 1));
    if (mprotect(page, size, PROT_READ) != 0) return JNI_FALSE;
    const jboolean readable = value == 7;
    const jboolean rejected = mprotect(page, size, PROT_READ | PROT_EXEC) != 0;
    if (mprotect(page, size, PROT_READ | PROT_WRITE) != 0) return JNI_FALSE;
    value = 8;
    const jboolean writable = value == 8;
    value = 7;
    return readable && rejected && writable;
}

static jboolean post_surface(JNIEnv* env, jclass unused, jobject surface) {
    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (!window) return JNI_FALSE;
    ANativeWindow_Buffer buffer;
    ARect dirty = {0, 0, 3, 2};
    jboolean ok = ANativeWindow_getWidth(window) == 3 && ANativeWindow_getHeight(window) == 2 &&
        ANativeWindow_getFormat(window) == WINDOW_FORMAT_RGBA_8888 &&
        ANativeWindow_lock(window, &buffer, &dirty) == 0;
    if (ok) {
        ok = buffer.width == 3 && buffer.height == 2 && buffer.stride >= 3 &&
            buffer.format == WINDOW_FORMAT_RGBA_8888 && buffer.bits;
        if (ok) ((uint32_t*)buffer.bits)[1] = UINT32_C(0xff776655);
        ok = ANativeWindow_unlockAndPost(window) == 0 && ok;
    }
    ANativeWindow_release(window);
    return ok;
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
    if (!verify_method_id(env) || !verify_non_movable_array(env) || !verify_library_paths()) return JNI_FALSE;
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
        {"editBitmap", "(Landroid/graphics/Bitmap;)Z", edit_bitmap},
        {"readAsset", "(Landroid/content/res/AssetManager;)Z", read_asset},
        {"postSurface", "(Landroid/view/Surface;)Z", post_surface},
        {"pollTimer", "()Z", poll_timer},
        {"protectImageData", "()Z", protect_image_data},
        {"wide", "(IIIIIIIIIJDDDDDDDDDDF)D", wide},
    };
    if (ENV(env, RegisterNatives, bridge, methods, sizeof(methods) / sizeof(methods[0])) != JNI_OK) return JNI_ERR;
    if (hybrid_hot_sum(65536) != 17 * 65536) return JNI_ERR;
    ++loads;
    __android_log_print(ANDROID_LOG_INFO, "HybridJNI", "JNI 1.6, constructor count %d", hybrid_constructor_count());
    return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM* vm, void* unused) {
    JNIEnv* env = NULL;
    if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) == JNI_OK && env) ENV(env, DeleteGlobalRef, bridge);
}
