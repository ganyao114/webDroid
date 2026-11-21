#include "ServiceManager.h"
#include "wa_binder_driver.h"

#include <android/os/BpServiceManager.h>
#include <binder/IPCThreadState.h>
#include <binder/ProcessState.h>
#include <org/wasmandroid/test/BnCalcService.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/mman.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#endif

namespace {

using android::BBinder;
using android::IBinder;
using android::IPCThreadState;
using android::ProcessState;
using android::ServiceManager;
using android::sp;
using android::String16;
using android::binder::Status;
using android::os::BpServiceManager;
using android::os::IServiceManager;
using org::wasmandroid::test::BnCalcService;
using org::wasmandroid::test::ICalcService;

constexpr size_t kRingBytes = 8 * 1024 * 1024;
constexpr wa_pid_t kManagerPid = 1;
constexpr wa_pid_t kServicePid = 2;
constexpr wa_pid_t kClientPid = 3;
constexpr char kServiceName[] = "org.wasmandroid.test.ICalcService";
constexpr int kRounds = 10;
constexpr int kCallsPerRound = 5;
constexpr int kCalls = kRounds * kCallsPerRound;

#define CHECK(condition)                                                             \
    do {                                                                             \
        if (!(condition)) {                                                          \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);       \
            std::abort();                                                            \
        }                                                                            \
    } while (false)

void checkStatus(const Status& status, const char* operation) {
    if (!status.isOk()) {
        std::fprintf(stderr, "%s failed: %s\n", operation, status.toString8().c_str());
        std::abort();
    }
}

class CalcService final : public BnCalcService {
  public:
    int serve() {
        IPCThreadState::self()->joinThreadPool();
        CHECK(calls_ == kCalls);
        return calls_;
    }

    Status add(int32_t lhs, int32_t rhs, int32_t* result) override {
        *result = lhs + rhs;
        return processed();
    }

    Status scale(int64_t value, int64_t factor, int64_t* result) override {
        *result = value * factor;
        return processed();
    }

    Status decorate(const String16& value, String16* result) override {
        *result = value;
        CHECK(result->append(u" \u2713", 2) == android::OK);
        return processed();
    }

    Status reverseBytes(const std::vector<uint8_t>& value, std::vector<uint8_t>* result) override {
        result->assign(value.rbegin(), value.rend());
        return processed();
    }

    Status echoBinder(const sp<IBinder>& value, sp<IBinder>* result) override {
        CHECK(value && value->remoteBinder() && !value->localBinder());
        *result = value;
        return processed();
    }

  private:
    Status processed() {
        CHECK(IPCThreadState::self()->getCallingPid() == kClientPid);
        CHECK(++calls_ <= kCalls);
        if (calls_ == kCalls) wa::aosp_binder::stopLooper();
        return Status::ok();
    }

    int calls_ = 0;
};

void startManager(void* ring) {
    CHECK(wa::aosp_binder::attach(ring, kRingBytes, kManagerPid) == WA_OK);
    const auto process = ProcessState::self();
    CHECK(process->setThreadPoolMaxThreadCount(0) == android::OK);
    const auto manager = sp<ServiceManager>::make(std::make_unique<android::Access>());
    IPCThreadState::self()->setTheContextObject(manager);
    CHECK(process->becomeContextManager());
}

void serveManager() {
    IPCThreadState::self()->joinThreadPool();
    CHECK(false && "ServiceManager looper exited early");
}

sp<BpServiceManager> managerProxy() {
    const auto binder = ProcessState::self()->getContextObject(nullptr);
    CHECK(binder && binder->remoteBinder() && !binder->localBinder());
    return sp<BpServiceManager>::make(binder);
}

sp<CalcService> registerCalc(void* ring) {
    CHECK(wa::aosp_binder::attach(ring, kRingBytes, kServicePid) == WA_OK);
    const auto manager = managerProxy();
    sp<IBinder> previous;
    checkStatus(manager->checkService(kServiceName, &previous), "checkService before registration");
    CHECK(!previous);
    const auto calc = sp<CalcService>::make();
    checkStatus(manager->addService(kServiceName, calc, false,
                                    IServiceManager::DUMP_FLAG_PRIORITY_DEFAULT), "addService");
    return calc;
}

int discoverAndCall(void* ring) {
    CHECK(wa::aosp_binder::attach(ring, kRingBytes, kClientPid) == WA_OK);
    const auto manager = managerProxy();

    std::vector<std::string> names;
    checkStatus(manager->listServices(IServiceManager::DUMP_FLAG_PRIORITY_ALL, &names), "listServices");
    CHECK(std::ranges::find(names, kServiceName) != names.end());
    std::vector<android::os::ServiceDebugInfo> debugInfo;
    checkStatus(manager->getServiceDebugInfo(&debugInfo), "getServiceDebugInfo");
    CHECK(std::ranges::any_of(debugInfo, [](const auto& info) {
        return info.name == kServiceName && info.debugPid == kServicePid;
    }));

    sp<IBinder> binder;
    checkStatus(manager->getService(kServiceName, &binder), "getService");
    CHECK(binder && binder->remoteBinder() && !binder->localBinder());
    sp<IBinder> checked;
    checkStatus(manager->checkService(kServiceName, &checked), "checkService");
    CHECK(checked == binder);

    android::os::Service service;
    checkStatus(manager->getService2(kServiceName, &service), "getService2");
    CHECK(service.getTag() == android::os::Service::Tag::serviceWithMetadata);
    const auto& metadata = service.get<android::os::Service::Tag::serviceWithMetadata>();
    CHECK(metadata.service == binder && !metadata.isLazyService);

    const auto calc = android::interface_cast<ICalcService>(binder);
    CHECK(calc);
    const auto token = sp<BBinder>::make();
    int calls = 0;
    for (int round = 0; round < kRounds; ++round) {
        int32_t sum = 0;
        checkStatus(calc->add(-1000 + round, 3 * round, &sum), "add");
        CHECK(sum == -1000 + 4 * round);

        const int64_t value = 0x100000000LL + round;
        int64_t product = 0;
        checkStatus(calc->scale(value, -3, &product), "scale");
        CHECK(product == -3 * value);

        String16 decorated;
        checkStatus(calc->decorate(String16(u"\u65e5\u672c \U0001f9ee"), &decorated), "decorate");
        CHECK(decorated == String16(u"\u65e5\u672c \U0001f9ee \u2713"));

        const auto marker = static_cast<uint8_t>(round);
        std::vector<uint8_t> reversed;
        checkStatus(calc->reverseBytes({0, marker, 255, 128, 17}, &reversed), "reverseBytes");
        CHECK(reversed == (std::vector<uint8_t>{17, 128, 255, marker, 0}));

        sp<IBinder> echoed;
        checkStatus(calc->echoBinder(token, &echoed), "echoBinder");
        CHECK(echoed == token);
        calls += kCallsPerRound;
    }
    return calls;
}

#ifdef __EMSCRIPTEN__
sp<CalcService> wasmCalc;
uint32_t memoryMarker;
#else
void signalReady(int fd) {
    CHECK(write(fd, "R", 1) == 1);
    close(fd);
}

class Child {
  public:
    Child() = default;
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;

    ~Child() {
        if (pid_ > 0) {
            kill(pid_, SIGTERM);
            wait();
        }
    }

    template <typename Run>
    bool start(Run run) {
        int ready[2];
        if (pipe(ready) != 0) {
            std::perror("pipe");
            return false;
        }
        pid_ = fork();
        if (pid_ == 0) {
            close(ready[0]);
            run(ready[1]);
            _exit(EXIT_SUCCESS);
        }
        if (pid_ < 0) std::perror("fork");
        close(ready[1]);
        char marker = 0;
        const ssize_t count = pid_ > 0 ? read(ready[0], &marker, 1) : -1;
        close(ready[0]);
        return count == 1 && marker == 'R';
    }

    bool wait() {
        int status = 0;
        pid_t result;
        do {
            result = waitpid(pid_, &status, 0);
        } while (result < 0 && errno == EINTR);
        pid_ = -1;
        return result > 0 && WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS;
    }

  private:
    pid_t pid_ = -1;
};
#endif

}  // namespace

#ifdef __EMSCRIPTEN__
extern "C" EMSCRIPTEN_KEEPALIVE uint32_t m5_sm_memory_marker(uint32_t marker) {
    return std::exchange(memoryMarker, marker);
}

extern "C" EMSCRIPTEN_KEEPALIVE void m5_sm_start_manager() {
    startManager(nullptr);
}

extern "C" EMSCRIPTEN_KEEPALIVE void m5_sm_serve_manager() {
    serveManager();
}

extern "C" EMSCRIPTEN_KEEPALIVE void m5_sm_register_calc() {
    wasmCalc = registerCalc(nullptr);
}

extern "C" EMSCRIPTEN_KEEPALIVE int m5_sm_serve_calc() {
    CHECK(wasmCalc);
    return wasmCalc->serve();
}

extern "C" EMSCRIPTEN_KEEPALIVE int m5_sm_discover_and_call() {
    return discoverAndCall(nullptr);
}
#else
int main() {
    // Fork before initializing ProcessState. Each process has its own handle
    // table and libbinder state, with only the M0 ring mapped into all three.
    void* ring = mmap(nullptr, kRingBytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(ring != MAP_FAILED);
    Child manager, service, client;
    if (!manager.start([&](int ready) {
            startManager(ring);
            signalReady(ready);
            serveManager();
        }) || !service.start([&](int ready) {
            std::thread([&] {
                const auto calc = registerCalc(ring);
                signalReady(ready);
                CHECK(calc->serve() == kCalls);
            }).join();
        }) || !client.start([&](int ready) {
            signalReady(ready);
            std::thread([&] { CHECK(discoverAndCall(ring) == kCalls); }).join();
        }) || !client.wait() || !service.wait()) {
        return EXIT_FAILURE;
    }
    CHECK(munmap(ring, kRingBytes) == 0);
    std::printf("PASS AOSP 17 servicemanager at PID 1/handle 0, %d exact typed Binder RPCs\n", kCalls);
    return EXIT_SUCCESS;
}
#endif
