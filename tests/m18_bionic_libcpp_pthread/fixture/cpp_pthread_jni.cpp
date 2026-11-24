#include <android/log.h>
#include <dlfcn.h>
#include <jni.h>
#include <link.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <csetjmp>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

std::string prefix = std::string("NDK ") + "C++";
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
pthread_once_t once = PTHREAD_ONCE_INIT;
pthread_key_t key;
sem_t gate;
int key_created, once_calls, key_destructors, cpp_destructors;
thread_local int instance = 37;
struct ThreadObject {
    int marker = 83;
    ~ThreadObject() { ++cpp_destructors; }
};
thread_local ThreadObject object;

void DestroyKey(void* value) {
    if (value)
        ++key_destructors;
}

void Once() {
    ++once_calls;
    key_created = pthread_key_create(&key, DestroyKey) == 0;
}

struct Exchange {
    int phase = 0;
    int error = 0;
    int* errno_slot = nullptr;
    int* instance_slot = nullptr;
    void* stack_local = nullptr;
};

void* Worker(void* opaque) {
    auto& exchange = *static_cast<Exchange*>(opaque);
    volatile int local = 31;
    exchange.errno_slot = &errno;
    exchange.instance_slot = &instance;
    exchange.stack_local = const_cast<int*>(&local);
    if (errno || instance != 37 || object.marker != 83 || pthread_once(&once, Once) ||
        pthread_getspecific(key))
        exchange.error = 1;
    errno = EDOM;
    instance = 41;
    object.marker = 89;
    if (pthread_setspecific(key, opaque) || pthread_mutex_lock(&mutex))
        exchange.error = 2;
    if (exchange.phase != 1)
        exchange.error = 3;
    exchange.phase = 2;
    pthread_cond_signal(&cond);
    while (exchange.phase != 3)
        if (pthread_cond_wait(&cond, &mutex)) {
            exchange.error = 4;
            break;
        }
    if (pthread_mutex_unlock(&mutex) || sem_wait(&gate) || pthread_getspecific(key) != opaque ||
        errno != EDOM || instance != 41 || local != 31)
        exchange.error = 5;
    pthread_exit(opaque);
}

int Threads() {
    constexpr int main_marker = 63;
    volatile int stack_local = 9;
    errno = main_marker;
    instance = 43;
    if (pthread_once(&once, Once) || !key_created || pthread_setspecific(key, &key) ||
        sem_init(&gate, 0, 0))
        return 1;
    if (pthread_mutex_lock(&mutex))
        return 2;
    Exchange exchange;
    pthread_t thread;
    pthread_attr_t attr;
    size_t stack_size = 0;
    if (pthread_attr_init(&attr) || pthread_attr_setstacksize(&attr, 64 * 1024) ||
        pthread_attr_getstacksize(&attr, &stack_size) || stack_size != 64 * 1024 ||
        pthread_create(&thread, &attr, Worker, &exchange) || pthread_attr_destroy(&attr))
        return 3;
    if (pthread_equal(pthread_self(), thread) || sched_yield())
        return 4;
    exchange.phase = 1;
    while (exchange.phase != 2)
        if (pthread_cond_wait(&cond, &mutex))
            return 5;
    exchange.phase = 3;
    if (pthread_cond_signal(&cond) || pthread_mutex_unlock(&mutex) || sched_yield() ||
        sem_post(&gate))
        return 6;
    void* result = nullptr;
    if (pthread_join(thread, &result) || result != &exchange || exchange.error ||
        errno != main_marker || instance != 43 || exchange.errno_slot == &errno ||
        exchange.instance_slot == &instance ||
        exchange.stack_local == const_cast<int*>(&stack_local) || key_destructors != 1 ||
        cpp_destructors != 1 || once_calls != 1 || pthread_getspecific(key) != &key ||
        pthread_key_delete(key) || sem_destroy(&gate))
        return 10 + exchange.error;

    pthread_mutexattr_t type;
    pthread_mutex_t other;
    if (pthread_mutexattr_init(&type) ||
        pthread_mutexattr_settype(&type, PTHREAD_MUTEX_RECURSIVE) ||
        pthread_mutex_init(&other, &type) || pthread_mutex_lock(&other) ||
        pthread_mutex_trylock(&other) || pthread_mutex_unlock(&other) ||
        pthread_mutex_unlock(&other) || pthread_mutex_destroy(&other) ||
        pthread_mutexattr_settype(&type, PTHREAD_MUTEX_ERRORCHECK) ||
        pthread_mutex_init(&other, &type) || pthread_mutex_lock(&other) ||
        pthread_mutex_lock(&other) != EDEADLK || pthread_mutex_unlock(&other) ||
        pthread_mutex_destroy(&other) || pthread_mutexattr_destroy(&type))
        return 20;

    pthread_rwlock_t rwlock = PTHREAD_RWLOCK_INITIALIZER;
    if (pthread_rwlock_rdlock(&rwlock) || pthread_rwlock_trywrlock(&rwlock) != EBUSY ||
        pthread_rwlock_unlock(&rwlock) || pthread_rwlock_wrlock(&rwlock) ||
        pthread_rwlock_unlock(&rwlock) || pthread_rwlock_destroy(&rwlock))
        return 21;

    pthread_condattr_t clock;
    pthread_cond_t timed;
    timespec now{};
    if (pthread_condattr_init(&clock) || pthread_condattr_setclock(&clock, CLOCK_MONOTONIC) ||
        pthread_cond_init(&timed, &clock) || pthread_condattr_destroy(&clock) ||
        clock_gettime(CLOCK_MONOTONIC, &now) || pthread_mutex_lock(&mutex) ||
        pthread_cond_timedwait(&timed, &mutex, &now) != ETIMEDOUT || pthread_mutex_unlock(&mutex) ||
        pthread_cond_destroy(&timed))
        return 22;
    return 0;
}

struct CheckImages {
    bool libcxx = false;
    bool fixture = false;
};

int Image(dl_phdr_info* info, size_t size, void* opaque) {
    auto& images = *static_cast<CheckImages*>(opaque);
    if (size < offsetof(dl_phdr_info, dlpi_phnum) + sizeof(info->dlpi_phnum) || !info->dlpi_addr ||
        !info->dlpi_phdr || !info->dlpi_phnum)
        return -1;
    bool load = false;
    for (int i = 0; i < info->dlpi_phnum; ++i)
        load |= info->dlpi_phdr[i].p_type == PT_LOAD;
    if (!load)
        return -1;
    images.libcxx |= std::strstr(info->dlpi_name, "libc++_shared.so") != nullptr;
    images.fixture |= std::strstr(info->dlpi_name, "libcpp_pthread_jni.so") != nullptr;
    return 0;
}

struct CountDestruction {
    int& count;
    ~CountDestruction() { ++count; }
};

[[gnu::noinline]] void Throw(int& destroyed) {
    const CountDestruction guard{destroyed};
    throw std::runtime_error("cpp-exc-ok");
}

int Posix() {
    int descriptors[2];
    if (pipe(descriptors))
        return 40;
    const char sent = 'P';
    char received = 0;
    if (write(descriptors[1], &sent, 1) != 1 || read(descriptors[0], &received, 1) != 1 ||
        received != sent || close(descriptors[0]) || close(descriptors[1]))
        return 41;

    const int event = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    uint64_t value = 9;
    if (event < 0 || write(event, &value, sizeof(value)) != sizeof(value))
        return 42;
    value = 0;
    if (read(event, &value, sizeof(value)) != sizeof(value) || value != 9 ||
        read(event, &value, sizeof(value)) != -1 || errno != EAGAIN || close(event))
        return 43;

    const auto page = getpagesize();
    void* allocation =
        mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (allocation == MAP_FAILED)
        return 44;
    auto* cell = static_cast<volatile int*>(allocation);
    *cell = 47;
    if (mprotect(allocation, page, PROT_READ) || *cell != 47 || munmap(allocation, page))
        return 45;

    std::jmp_buf jump;
    const auto result = setjmp(jump);
    if (!result)
        longjmp(jump, 51);
    if (result != 51)
        return 46;

    wchar_t wide[16];
    if (std::swprintf(wide, 16, L"%ls %.1Lf", L"\u03bb", 1.5L) != 5 ||
        std::wmemcmp(wide, L"\u03bb 1.5", 6))
        return 47;
    return 0;
}

jint Run(JNIEnv* env, jobject receiver) {
    std::vector<std::string> words{prefix, "containers", "OK"};
    std::unordered_map<std::string, size_t> lookup{{words[0], 0}, {words[1], 1}, {words[2], 2}};
    std::ostringstream stream;
    for (const auto& word : words)
        stream << (lookup.at(word) ? " " : "") << word;
    char text[64];
    if (std::snprintf(text, sizeof(text), "%s %d", stream.str().c_str(), 24) != 24 ||
        std::strcmp(text, "NDK C++ containers OK 24"))
        return 30;

    CheckImages images;
    if (dl_iterate_phdr(Image, &images) || !images.libcxx || !images.fixture || fileno(stderr) != 2)
        return 31;

    int destroyed = 0;
    std::string caught;
    try {
        Throw(destroyed);
    } catch (const std::exception& error) {
        caught = error.what();
    }
    if (caught != "cpp-exc-ok" || destroyed != 1)
        return 32;

    if (const auto code = Threads()) {
        __android_log_print(ANDROID_LOG_ERROR, "cpp-pthread", "pthread diagnostic %d", code);
        return code;
    }

    FILE* file = std::tmpfile();
    int value = 0;
    double score = 0;
    if (!file || std::fprintf(file, "%d %.2f", 27, 3.25) != 7 || std::fseek(file, 0, SEEK_SET) ||
        std::fscanf(file, "%d %lf", &value, &score) != 2 || value != 27 || score != 3.25 ||
        std::fclose(file))
        return 33;

    if (const auto code = Posix()) {
        __android_log_print(ANDROID_LOG_ERROR, "cpp-pthread", "POSIX diagnostic %d", code);
        return code;
    }

    jclass cls = env->GetObjectClass(receiver);
    jmethodID report = env->GetMethodID(
        cls, "onNativeReport",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
    if (!report)
        return 34;
    env->CallVoidMethod(receiver, report, env->NewStringUTF(stream.str().c_str()),
                        env->NewStringUTF(caught.c_str()),
                        env->NewStringUTF("pthread TLS and handoff OK"),
                        env->NewStringUTF("Guest stdio and ELF unwind OK"));
    return 0;
}

} // namespace

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK)
        return JNI_ERR;
    jclass activity = env->FindClass("com/wasmandroid/cpppthread/CppActivity");
    if (!activity)
        return JNI_ERR;
    const JNINativeMethod methods[] = {{"runChecks", "()I", reinterpret_cast<void*>(Run)}};
    return env->RegisterNatives(activity, methods, 1) == JNI_OK ? JNI_VERSION_1_6 : JNI_ERR;
}
