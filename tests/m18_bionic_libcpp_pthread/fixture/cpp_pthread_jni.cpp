#include <android/log.h>
#include <dlfcn.h>
#include <jni.h>
#include <link.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <unistd.h>
#include <zlib.h>

#include <cerrno>
#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
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
    constexpr size_t supplied_size = 130 * 1024 * 1024;
    auto* supplied_stack = mmap(nullptr, supplied_size, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    size_t stack_size = 0;
    void* stack_base = nullptr;
    if (supplied_stack == MAP_FAILED || pthread_attr_init(&attr) || pthread_attr_setstacksize(&attr, 64 * 1024) ||
        pthread_attr_getstacksize(&attr, &stack_size) || stack_size != 64 * 1024 ||
        pthread_attr_setstack(&attr, supplied_stack, supplied_size) ||
        pthread_attr_getstack(&attr, &stack_base, &stack_size) ||
        stack_base != supplied_stack || stack_size != supplied_size ||
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
    const auto worker_stack = reinterpret_cast<uintptr_t>(exchange.stack_local);
    const auto mapped_stack = reinterpret_cast<uintptr_t>(supplied_stack);
    if (worker_stack < mapped_stack || worker_stack - mapped_stack >= supplied_size ||
        munmap(supplied_stack, supplied_size))
        return 23;

    pthread_mutexattr_t type;
    pthread_mutex_t other;
    if (pthread_mutexattr_init(&type) ||
        pthread_mutexattr_settype(&type, PTHREAD_MUTEX_RECURSIVE) ||
        pthread_mutex_init(&other, &type) || pthread_mutex_lock(&other) ||
        pthread_mutex_trylock(&other) || pthread_mutex_unlock(&other) ||
        pthread_mutex_unlock(&other) || pthread_mutex_destroy(&other) ||
        pthread_mutex_lock(&other) != EINVAL ||
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
    if (tgkill(getpid(), gettid(), 0) || tgkill(getpid(), -1, 0) != -1 || errno != EINVAL)
        return 24;
    tm parsed{};
    parsed.tm_sec = 37;
    const auto* remaining = strptime("2026-04-05tail", "%Y-%m-%d", &parsed);
    if (!remaining || std::strcmp(remaining, "tail") || parsed.tm_year != 126 || parsed.tm_mon != 3 ||
        parsed.tm_mday != 5 || parsed.tm_sec != 37)
        return 25;
    return 0;
}

JavaVM* java_vm;
pthread_t reference_worker;
sem_t reference_ready, reference_release;

void* ReferencingThread(void*) {
    JNIEnv* env = nullptr;
    bool valid = java_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED &&
                 java_vm->AttachCurrentThread(&env, nullptr) == JNI_OK;
    jclass integer = nullptr;
    if (valid && env->PushLocalFrame(1) == JNI_OK) {
        integer = env->FindClass("java/lang/Integer");
        integer = static_cast<jclass>(env->PopLocalFrame(integer));
    }
    if (sem_post(&reference_ready) || sem_wait(&reference_release))
        return reinterpret_cast<void*>(uintptr_t{1});
    valid = valid && integer && env->GetObjectRefType(integer) == JNILocalRefType &&
            env->GetMethodID(integer, "intValue", "()I") != nullptr;
    if (valid) env->DeleteLocalRef(integer);
    valid = java_vm->DetachCurrentThread() == JNI_OK && valid;
    return valid ? nullptr : reinterpret_cast<void*>(uintptr_t{1});
}

jint BeginReferenceTest(JNIEnv*, jobject) {
    return sem_init(&reference_ready, 0, 0) || sem_init(&reference_release, 0, 0) ||
           pthread_create(&reference_worker, nullptr, ReferencingThread, nullptr) || sem_wait(&reference_ready);
}

jint FinishReferenceTest(JNIEnv*, jobject) {
    void* result = nullptr;
    return sem_post(&reference_release) || pthread_join(reference_worker, &result) ||
           sem_destroy(&reference_ready) || sem_destroy(&reference_release) || result;
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

#if defined(__aarch64__)
int SparseMappings(size_t page) {
    constexpr size_t size = size_t{20} << 30;
    constexpr int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
    auto* mapping = static_cast<char*>(mmap(nullptr, size, PROT_NONE, flags, -1, 0));
    if (mapping == MAP_FAILED || reinterpret_cast<uintptr_t>(mapping) < (uint64_t{1} << 32)) return 60;
    auto* region = mapping + size - 3 * page;
    unsigned char resident[3]{};
    if (mprotect(region, 3 * page, PROT_READ | PROT_WRITE) ||
        mincore(region, 3 * page, resident) || resident[0] || resident[1] || resident[2]) return 61;
    auto* cell = reinterpret_cast<volatile int*>(region + 2 * page);
    if (*cell != 0) return 62;
    std::memset(region + 2 * page, 'a', page);
    if (mincore(region, 3 * page, resident) || resident[0] || resident[1] || !resident[2]) return 63;
    if (mmap(region + 2 * page, page, PROT_NONE, flags | MAP_FIXED_NOREPLACE, -1, 0) != MAP_FAILED ||
        errno != EEXIST) return 64;
    if (mmap(region + 2 * page, page, PROT_READ | PROT_WRITE, flags | MAP_FIXED, -1, 0) != region + 2 * page ||
        *cell != 0) return 65;
    if (munmap(region + page, page) || mprotect(region, 3 * page, PROT_READ) != -1 || errno != ENOMEM)
        return 66;
    constexpr uint32_t code[]{0x52800540, 0xd65f03c0}; // mov w0, #42 and ret.
    std::memcpy(region, code, sizeof(code));
    if (mprotect(region, page, PROT_READ | PROT_EXEC) || reinterpret_cast<int (*)()>(region)() != 42)
        return 67;
    *cell = 42;
    if (madvise(region + 2 * page, page, MADV_DONTNEED) || *cell || munmap(mapping, size)) return 68;
    return 0;
}
#endif

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
    const int epoll = epoll_create1(EPOLL_CLOEXEC);
    epoll_event watch{}, ready{};
    watch.events = EPOLLIN | EPOLLONESHOT;
    watch.data.u64 = 0x88aabbcc11223344ULL;
    if (event < 0 || epoll < 0 || epoll_ctl(epoll, EPOLL_CTL_ADD, event, &watch) ||
        epoll_wait(epoll, &ready, 1, 0) != 0) return 56;
    uint64_t value = 9;
    if (event < 0 || write(event, &value, sizeof(value)) != sizeof(value))
        return 42;
    if (epoll_wait(epoll, &ready, 1, 0) != 1 || ready.events != EPOLLIN ||
        ready.data.u64 != watch.data.u64 || epoll_wait(epoll, &ready, 1, 0) != 0 ||
        epoll_ctl(epoll, EPOLL_CTL_MOD, event, &watch) || epoll_wait(epoll, &ready, 1, 0) != 1 ||
        close(epoll)) return 57;
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
#if defined(__aarch64__)
    if (const auto code = SparseMappings(page)) return code;
#endif

    FILE* backed = std::tmpfile();
    const int fd = backed ? fileno(backed) : -1;
    if (fd < 0 || ftruncate(fd, page + 4) || flock(fd, LOCK_EX | LOCK_NB)) return 48;
    auto* writer = static_cast<volatile char*>(mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, page));
    auto* reader = static_cast<volatile char*>(mmap(nullptr, page, PROT_READ, MAP_SHARED, fd, page));
    if (writer == MAP_FAILED || reader == MAP_FAILED) return 49;
    writer[0] = 'M';
    writer[page - 1] = 'X';  // The partial EOF page cannot grow the file.
    if (reader[0] != 'M' || pwrite(fd, "K", 1, page + 1) != 1 ||
        msync(const_cast<char*>(writer), page, MS_SYNC | MS_INVALIDATE) ||
        reader[1] != 'K') return 50;
    writer[2] = 'V';
    char persisted[4]{};
    if (munmap(const_cast<char*>(writer), page) || reader[2] != 'V' ||
        pread(fd, persisted, sizeof(persisted), page) != sizeof(persisted) ||
        std::memcmp(persisted, "MKV", sizeof(persisted)) ||
        lseek(fd, 0, SEEK_END) != page + 4 || flock(fd, LOCK_UN) ||
        std::fclose(backed) || munmap(const_cast<char*>(reader), page)) return 51;

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

    unsigned char input[] = "ARM guest zlib stream";
    unsigned char zipped[128]{}, output[sizeof(input)]{};
    z_stream writer_stream{};
    if (deflateInit2(&writer_stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY))
        return 52;
    writer_stream.next_in = input;
    writer_stream.avail_in = sizeof(input);
    writer_stream.next_out = zipped;
    writer_stream.avail_out = sizeof(zipped);
    if (deflate(&writer_stream, Z_FINISH) != Z_STREAM_END ||
        writer_stream.total_in != sizeof(input) || deflateEnd(&writer_stream))
        return 53;
    z_stream reader_stream{};
    if (inflateInit2(&reader_stream, 15 + 16)) return 54;
    reader_stream.next_in = zipped;
    reader_stream.avail_in = writer_stream.total_out;
    reader_stream.next_out = output;
    reader_stream.avail_out = sizeof(output);
    if (inflate(&reader_stream, Z_FINISH) != Z_STREAM_END ||
        reader_stream.total_out != sizeof(output) || inflateEnd(&reader_stream) ||
        std::memcmp(input, output, sizeof(input)))
        return 55;
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
    java_vm = vm;
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK)
        return JNI_ERR;
    jclass activity = env->FindClass("com/wasmandroid/cpppthread/CppActivity");
    if (!activity)
        return JNI_ERR;
    const JNINativeMethod methods[] = {
        {"runChecks", "()I", reinterpret_cast<void*>(Run)},
        {"beginReferenceTest", "()I", reinterpret_cast<void*>(BeginReferenceTest)},
        {"finishReferenceTest", "()I", reinterpret_cast<void*>(FinishReferenceTest)},
    };
    return env->RegisterNatives(activity, methods, sizeof(methods) / sizeof(*methods)) == JNI_OK
        ? JNI_VERSION_1_6 : JNI_ERR;
}
