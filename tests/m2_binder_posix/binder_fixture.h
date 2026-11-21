#pragma once

#include "wasmandroid/binder.h"

#include <array>
#include <bit>

namespace m2 {

using namespace wa::binder;

inline constexpr size_t kRingBytes = 16 * 1024 * 1024;
inline constexpr int32_t kCalls = 200;
inline constexpr int32_t kManagerCalls = 6;
inline constexpr char kServiceName[] = "android.test.ICalc";
inline constexpr std::u16string_view kCalcDescriptor = u"android.test.ICalc";
inline constexpr std::u16string_view kCallbackDescriptor = u"android.test.ICalcCallback";
inline constexpr uint32_t kCalculate = IBinder::FIRST_CALL_TRANSACTION;
inline constexpr uint32_t kSyncCalculate = kCalculate + 1;

struct Values {
    int32_t index;
    int64_t wide;
    float fraction;
    std::u16string text;
    std::vector<uint8_t> bytes;

    bool operator==(const Values& other) const {
        return index == other.index && wide == other.wide &&
               std::bit_cast<uint32_t>(fraction) == std::bit_cast<uint32_t>(other.fraction) &&
               text == other.text && bytes == other.bytes;
    }
};

inline Values requestValues(int32_t index) {
    constexpr char16_t sample[] = u"\u03c0\U0001F9EE\0\u7b97";
    Values values{index,
                  -0x1234'5678'9000LL + 137 * index,
                  (index - 95) * 0.125f,
                  std::u16string(sample, std::size(sample) - 1),
                  {}};
    values.text.push_back(static_cast<char16_t>(0x100 + index));
    for (int32_t i = 0; i < index % 9; ++i) {
        values.bytes.push_back(static_cast<uint8_t>(index * 17 + i * 13));
    }
    return values;
}

inline Values replyValues(Values values) {
    values.wide *= -3;
    values.fraction += 0.25f;
    values.text += u" ✓";
    values.bytes.push_back(0);
    values.bytes.push_back(static_cast<uint8_t>(values.index ^ 0xa5));
    values.index -= 300;
    return values;
}

inline bool putValues(Parcel& parcel, const Values& values) {
    return parcel.writeInt32(values.index) == WA_OK && parcel.writeInt64(values.wide) == WA_OK &&
           parcel.writeFloat(values.fraction) == WA_OK &&
           parcel.writeString16(values.text) == WA_OK &&
           parcel.writeByteArray(values.bytes) == WA_OK;
}

inline bool getValues(const Parcel& parcel, Values& values) {
    return parcel.readInt32(values.index) == WA_OK && parcel.readInt64(values.wide) == WA_OK &&
           parcel.readFloat(values.fraction) == WA_OK &&
           parcel.readString16(values.text) == WA_OK && parcel.readByteArray(values.bytes) == WA_OK;
}

inline bool makeRequest(Parcel& request, int32_t index, const std::shared_ptr<IBinder>& callback) {
    return request.writeInterfaceToken(kCalcDescriptor) == WA_OK &&
           putValues(request, requestValues(index)) && request.writeStrongBinder(callback) == WA_OK;
}

inline bool exactReply(const Parcel& reply, int32_t index,
                       const std::shared_ptr<IBinder>& callback) {
    Values actual{};
    std::shared_ptr<IBinder> returned;
    return getValues(reply, actual) && actual == replyValues(requestValues(index)) &&
           reply.readStrongBinder(returned) == WA_OK && returned == callback && !reply.dataAvail();
}

class Callback final : public BBinder {
  public:
    bool complete() const { return count_.load() == kCalls + 1 && errors_.load() == 0; }

  private:
    int32_t onTransact(uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags) override {
        int32_t index;
        int64_t proof;
        if (code != kCalculate || !data.enforceInterface(kCallbackDescriptor) ||
            data.readInt32(index) != WA_OK || index < 0 || index > kCalls ||
            data.readInt64(proof) != WA_OK || proof != requestValues(index).wide ||
            data.dataAvail() ||
            (index == kCalls ? (!reply || flags != 0) : (reply || flags != FLAG_ONEWAY)) ||
            received_[index].exchange(true)) {
            ++errors_;
            return WA_ERR_INVALID;
        }
        ++count_;
        return reply ? reply->writeInt64(-proof) : WA_OK;
    }

    std::array<std::atomic_bool, kCalls + 1> received_{};
    std::atomic<int32_t> count_{0};
    std::atomic<int32_t> errors_{0};
};

class Calc final : public BBinder {
  public:
    int32_t calls() const { return calls_.load(); }

  private:
    int32_t onTransact(uint32_t code, const Parcel& request, Parcel* reply,
                       uint32_t flags) override {
        if (code != kCalculate && code != kSyncCalculate) {
            return WA_ERR_NOSYS;
        }
        Values values{};
        std::shared_ptr<IBinder> callback;
        if (!reply || flags || !request.enforceInterface(kCalcDescriptor) ||
            !getValues(request, values) || values.index < 0 || values.index > kCalls ||
            (values.index == kCalls) != (code == kSyncCalculate) ||
            values != requestValues(values.index) || request.readStrongBinder(callback) != WA_OK ||
            !callback || callback->handle().owner_pid != 3 || request.dataAvail()) {
            return WA_ERR_INVALID;
        }
        Parcel call, answer;
        if (call.writeInterfaceToken(kCallbackDescriptor) != WA_OK ||
            call.writeInt32(values.index) != WA_OK || call.writeInt64(values.wide) != WA_OK) {
            return WA_ERR_NOMEM;
        }
        const bool synchronous = code == kSyncCalculate;
        const int32_t status =
            callback->transact(kCalculate, call, &answer, synchronous ? 0 : FLAG_ONEWAY);
        if (status != WA_OK) {
            return status;
        }
        int64_t proof;
        if (synchronous &&
            (answer.readInt64(proof) != WA_OK || proof != -values.wide || answer.dataAvail())) {
            return WA_ERR_INVALID;
        }
        if (!putValues(*reply, replyValues(std::move(values))) ||
            reply->writeStrongBinder(callback) != WA_OK) {
            return WA_ERR_NOMEM;
        }
        ++calls_;
        return WA_OK;
    }

    std::atomic<int32_t> calls_{0};
};

inline std::shared_ptr<BpBinder> lookupService() {
    auto manager = defaultServiceManager();
    int32_t status;
    auto binder = manager->getService(kServiceName, &status);
    if (!binder || status != WA_OK || binder->handle().owner_pid != 2 ||
        !binder->handle().handle_id ||
        manager->listServices(15, &status) != std::vector<std::string>{kServiceName} ||
        status != WA_OK) {
        return nullptr;
    }
    auto checked = manager->checkService(kServiceName, &status);
    if (!checked || status != WA_OK || checked->handle() != binder->handle() ||
        manager->checkService("android.test.Missing", &status) || status != WA_OK ||
        !manager->listServices(1, &status).empty() || status != WA_OK) {
        return nullptr;
    }
    return binder;
}

inline bool doSyncCall(BpBinder& binder, const std::shared_ptr<Callback>& callback) {
    Parcel request, response;
    return makeRequest(request, kCalls, callback) &&
           binder.transact(kSyncCalculate, request, &response) == WA_OK &&
           exactReply(response, kCalls, callback);
}

} // namespace m2
