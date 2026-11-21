#include "wa_binder_driver.h"

#include <binder/IPCThreadState.h>
#include <binder/ProcessState.h>
#include <binder/Stability.h>
#include <org/wasmandroid/test/BnCalcService.h>
#include <org/wasmandroid/test/BpCalcService.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <utility>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

namespace {

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                   \
            std::abort();                                                                          \
        }                                                                                          \
    } while (false)

using android::BBinder;
using android::IBinder;
using android::IPCThreadState;
using android::Parcel;
using android::ProcessState;
using android::sp;
using android::String16;
using android::binder::Status;
using org::wasmandroid::test::BnCalcService;
using org::wasmandroid::test::BpCalcService;
using wa::aosp_binder::Endpoint;

constexpr size_t kRingBytes = 8 * 1024 * 1024;
constexpr wa_pid_t kLocalPid = 10;
constexpr wa_pid_t kServerPid = 21;
constexpr wa_pid_t kClientPid = 22;
constexpr int kRounds = 10;
constexpr int kCallsPerRound = 5;
constexpr int kCalls = kCallsPerRound * kRounds;

void checkParcel() {
    Parcel parcel;
    const String16 token(u"org.wasmandroid.test.Parcel");
    const String16 utf16(u"Binder \u4e16\u754c \U0001f9ee");
    const std::string utf8 = "Binder \xe2\x86\x94 Wasm \xf0\x9f\xa7\xae";
    const std::vector<uint8_t> bytes{0, 17, 127, 128, 255};
    const auto binder = sp<BBinder>::make();
    constexpr int32_t number = -2077;
    constexpr int64_t large = -0x123456789ABCDEF;
    constexpr float real = 7.625f;

    CHECK(parcel.writeInterfaceToken(token) == android::OK);
    CHECK(parcel.writeInt32(number) == android::OK);
    CHECK(parcel.writeInt64(large) == android::OK);
    CHECK(parcel.writeFloat(real) == android::OK);
    CHECK(parcel.writeString16(utf16) == android::OK);
    CHECK(parcel.writeUtf8AsUtf16(utf8) == android::OK);
    CHECK(parcel.writeByteVector(bytes) == android::OK);
    CHECK(parcel.writeStrongBinder(binder) == android::OK);

    parcel.setDataPosition(0);
    CHECK(parcel.enforceInterface(token));
    CHECK(parcel.readInt32() == number);
    CHECK(parcel.readInt64() == large);
    CHECK(parcel.readFloat() == real);
    CHECK(parcel.readString16() == utf16);
    std::string readUtf8;
    CHECK(parcel.readUtf8FromUtf16(&readUtf8) == android::OK && readUtf8 == utf8);
    std::vector<uint8_t> readBytes;
    CHECK(parcel.readByteVector(&readBytes) == android::OK && readBytes == bytes);
    sp<IBinder> readBinder;
    CHECK(parcel.readStrongBinder(&readBinder) == android::OK && readBinder == binder);
    CHECK(parcel.dataAvail() == 0);

    Parcel statusParcel;
    const auto error = Status::fromServiceSpecificError(42, android::String8("calculation error"));
    CHECK(error.writeToParcel(&statusParcel) == android::OK);
    statusParcel.setDataPosition(0);
    Status readStatus;
    CHECK(readStatus.readFromParcel(statusParcel) == android::OK);
    CHECK(readStatus.exceptionCode() == Status::EX_SERVICE_SPECIFIC &&
          readStatus.serviceSpecificErrorCode() == 42 &&
          readStatus.exceptionMessage() == android::String8("calculation error"));
}

class Token final : public BBinder {
  public:
    explicit Token(bool crossProcess) : crossProcess_(crossProcess) {}

    android::status_t pingBinder() override {
        if (crossProcess_)
            CHECK(IPCThreadState::self()->getCallingPid() == kServerPid);
        ++pings;
        return android::OK;
    }

    std::atomic<int> pings{0};

  private:
    const bool crossProcess_;
};

class CalcService final : public BnCalcService {
  public:
    CalcService(wa_pid_t ownPid, wa_pid_t clientPid) : ownPid_(ownPid), clientPid_(clientPid) {}

    int run() {
        looper_ = std::this_thread::get_id();
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
        CHECK(value != nullptr);
        if (ownPid_ != clientPid_)
            CHECK(value->remoteBinder() != nullptr);
        CHECK(value->pingBinder() == android::OK);
        *result = value;
        return processed();
    }

  private:
    Status processed() {
        CHECK(std::this_thread::get_id() == looper_);
        CHECK(IPCThreadState::self()->getCallingPid() == static_cast<pid_t>(clientPid_));
        CHECK(++calls_ <= kCalls);
        if (calls_ == kCalls)
            wa::aosp_binder::stopLooper();
        return Status::ok();
    }

    const wa_pid_t ownPid_;
    const wa_pid_t clientPid_;
    int calls_ = 0;
    std::thread::id looper_;
};

sp<BpCalcService> connect(Endpoint endpoint) {
    const auto handle = wa::aosp_binder::handleFor(endpoint);
    CHECK(handle > 0);
    const auto binder = ProcessState::self()->getStrongProxyForHandle(handle);
    CHECK(binder && binder->remoteBinder() && !binder->localBinder());
    // The service endpoint is bootstrapped out of band, like the context
    // object, so there was no Parcel carrying its stability.
    android::internal::Stability::markCompilationUnit(binder.get());
    return sp<BpCalcService>::make(binder);
}

int callService(const sp<BpCalcService>& service, bool crossProcess, int first, int end) {
    const auto token = sp<Token>::make(crossProcess);
    for (int round = first; round < end; ++round) {
        int32_t sum = 0;
        CHECK(service->add(-1000 + round, 3 * round, &sum).isOk());
        CHECK(sum == -1000 + 4 * round);

        const int64_t value = 0x100000000LL + round;
        int64_t product = 0;
        CHECK(service->scale(value, -3, &product).isOk());
        CHECK(product == -3 * value);

        String16 decorated;
        CHECK(service->decorate(String16(u"\u65e5\u672c \U0001f9ee"), &decorated).isOk());
        CHECK(decorated == String16(u"\u65e5\u672c \U0001f9ee \u2713"));

        const auto marker = static_cast<uint8_t>(round);
        std::vector<uint8_t> reversed;
        CHECK(service->reverseBytes({0, marker, 255, 128, 17}, &reversed).isOk());
        CHECK(reversed == (std::vector<uint8_t>{17, 128, 255, marker, 0}));

        sp<IBinder> returned;
        CHECK(service->echoBinder(token, &returned).isOk() && returned == token);
    }
    CHECK(token->pings == end - first);
    return (end - first) * kCallsPerRound;
}

int localTest() {
    alignas(8) static std::array<uint8_t, kRingBytes> ring{};
    CHECK(wa::aosp_binder::attach(ring.data(), ring.size(), kLocalPid) == WA_OK);
    checkParcel();

    const auto service = sp<CalcService>::make(kLocalPid, kLocalPid);
    const auto endpoint = wa::aosp_binder::publish(service);
    CHECK(endpoint.pid == kLocalPid && endpoint.node);
    const auto proxy = connect(endpoint);
    std::thread server([&] { CHECK(service->run() == kCalls); });
    std::thread first([&] { CHECK(callService(proxy, false, 0, kRounds / 2) == kCalls / 2); });
    std::thread second(
        [&] { CHECK(callService(proxy, false, kRounds / 2, kRounds) == kCalls / 2); });
    first.join();
    second.join();
    server.join();
    std::printf("PASS upstream Parcel and Status roundtrips, %d generated AIDL RPCs over M0\n",
                kCalls);
    return kCalls;
}

} // namespace

#ifdef __EMSCRIPTEN__
namespace {
sp<CalcService> crossProcessService;
uint32_t memoryMarker;
} // namespace

extern "C" EMSCRIPTEN_KEEPALIVE uint32_t m4_memory_marker(uint32_t marker) {
    return std::exchange(memoryMarker, marker);
}

extern "C" EMSCRIPTEN_KEEPALIVE int m4_local() {
    return localTest();
}

extern "C" EMSCRIPTEN_KEEPALIVE uint32_t m4_server_init() {
    CHECK(wa::aosp_binder::attach(nullptr, kRingBytes, kServerPid) == WA_OK);
    crossProcessService = sp<CalcService>::make(kServerPid, kClientPid);
    const auto endpoint = wa::aosp_binder::publish(crossProcessService);
    CHECK(endpoint.pid == kServerPid && endpoint.node);
    return endpoint.node;
}

extern "C" EMSCRIPTEN_KEEPALIVE int m4_server_run() {
    CHECK(crossProcessService);
    return crossProcessService->run();
}

extern "C" EMSCRIPTEN_KEEPALIVE int m4_client_run(uint32_t node) {
    CHECK(wa::aosp_binder::attach(nullptr, kRingBytes, kClientPid) == WA_OK);
    return callService(connect({kServerPid, node}), true, 0, kRounds);
}
#else
int main() {
    return localTest() == kCalls ? EXIT_SUCCESS : EXIT_FAILURE;
}
#endif
