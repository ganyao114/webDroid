#include "wasmandroid/binder.h"

#include "kernel_internal.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <mutex>
#include <new>
#include <unordered_map>
#include <utility>

namespace wa::binder {
namespace {

constexpr uint32_t kReply = 0x80000000;
constexpr int32_t kSendTimeoutMs = 5000;
constexpr int32_t kPollIntervalMs = 5;
constexpr size_t kEnvelopeSize = sizeof(uint32_t);

thread_local BinderProcess* attached_process;

int32_t sendPacket(wa_pid_t peer, uint32_t id, uint32_t code, uint32_t flags, uint32_t word,
                   const Parcel& parcel) {
    const auto data = parcel.data();
    if (data.size() > WA_BINDER_MAX_PAYLOAD - kEnvelopeSize) {
        return WA_ERR_OVERFLOW;
    }
    std::vector<uint8_t> packet;
    try {
        packet.resize(kEnvelopeSize + data.size());
    } catch (const std::bad_alloc&) {
        return WA_ERR_NOMEM;
    }
    std::memcpy(packet.data(), &word, kEnvelopeSize);
    std::copy(data.begin(), data.end(), packet.begin() + kEnvelopeSize);

    const Deadline deadline(kSendTimeoutMs);
    uint32_t pause = 0;
    for (;;) {
        const int32_t status = wa_binder_send(peer, id, code, flags, packet.data(), packet.size());
        if (status != WA_ERR_AGAIN) {
            return status;
        }
        const int64_t remaining = deadline.remaining();
        if (!remaining) {
            return WA_ERR_TIMEDOUT;
        }
        wa_futex_wait(&pause, 0, std::min<int64_t>(remaining, 1'000'000));
    }
}

} // namespace

struct BinderProcess::Impl {
    struct Pending {
        wa_pid_t peer;
        uint32_t code;
        bool complete = false;
        int32_t status = WA_OK;
        Parcel reply;
    };

    FutexMutex mutex;
    uint32_t next_handle = 1;
    uint32_t next_transaction = 1;
    std::unordered_map<uint32_t, std::shared_ptr<BBinder>> objects;
    std::unordered_map<uint32_t, Pending> pending;
};

BinderHandle BBinder::handle() const {
    const uint64_t address = address_.load(std::memory_order_acquire);
    return {static_cast<wa_pid_t>(address >> 32), static_cast<uint32_t>(address)};
}

int32_t BBinder::transact(uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags) {
    if (flags & kReply) {
        return WA_ERR_INVALID;
    }
    try {
        Parcel request(data), response;
        request.setDataPosition(0);
        const int32_t status =
            onTransact(code, request, flags & FLAG_ONEWAY ? nullptr : &response, flags);
        if (status == WA_OK && reply && !(flags & FLAG_ONEWAY)) {
            response.setDataPosition(0);
            *reply = std::move(response);
        }
        return status;
    } catch (const std::bad_alloc&) {
        return WA_ERR_NOMEM;
    } catch (...) {
        return WA_ERR_INTERNAL;
    }
}

int32_t BBinder::onTransact(uint32_t, const Parcel&, Parcel*, uint32_t) {
    return WA_ERR_NOSYS;
}

BpBinder::BpBinder(BinderProcess& process, BinderHandle handle)
    : process_(process), handle_(handle) {}

int32_t BpBinder::transact(uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags) {
    if (flags & FLAG_ONEWAY) {
        return process_.send(handle_, code, data, flags, nullptr);
    }
    BinderTransaction transaction;
    const int32_t status = beginTransact(code, data, transaction, flags);
    return status == WA_OK ? transaction.wait(reply) : status;
}

int32_t BpBinder::beginTransact(uint32_t code, const Parcel& data, BinderTransaction& transaction,
                                uint32_t flags) {
    if (flags & FLAG_ONEWAY) {
        return WA_ERR_INVALID;
    }
    return process_.send(handle_, code, data, flags, &transaction);
}

BinderTransaction::~BinderTransaction() {
    reset();
}

BinderTransaction::BinderTransaction(BinderTransaction&& other) noexcept
    : process_(std::exchange(other.process_, nullptr)), id_(other.id_) {}

BinderTransaction& BinderTransaction::operator=(BinderTransaction&& other) noexcept {
    if (this != &other) {
        reset();
        process_ = std::exchange(other.process_, nullptr);
        id_ = other.id_;
    }
    return *this;
}

void BinderTransaction::reset() {
    if (auto* process = std::exchange(process_, nullptr)) {
        process->abandon(id_);
    }
}

int32_t BinderTransaction::wait(Parcel* reply, int32_t timeout_ms) {
    if (!process_ || process_ != BinderProcess::current()) {
        return WA_ERR_BADF;
    }
    const int32_t status = process_->wait(id_, reply, timeout_ms);
    reset();
    return status;
}

BinderProcess::BinderProcess(wa_pid_t pid, void* shared_ring_base, size_t ring_bytes)
    : impl_(std::make_unique<Impl>()), pid_(pid), ring_base_(shared_ring_base),
      ring_bytes_(ring_bytes) {}

BinderProcess::~BinderProcess() {
    if (attached_process == this) {
        attached_process = nullptr;
    }
    for (const auto& [id, binder] : impl_->objects) {
        binder->address_.store(0, std::memory_order_release);
    }
}

BinderProcess* BinderProcess::current() {
    return attached_process;
}

int32_t BinderProcess::attachThread() {
    const int32_t status = wa_binder_init_ring(ring_base_, ring_bytes_, pid_);
    if (status == WA_OK) {
        attached_process = this;
    }
    return status;
}

int32_t BinderProcess::registerBinder(const std::shared_ptr<BBinder>& binder) {
    return publish(binder, false);
}

int32_t BinderProcess::publish(const std::shared_ptr<BBinder>& binder, bool context_manager) {
    if (!binder || !pid_ || (context_manager && pid_ != WA_SERVICEMANAGER_PID)) {
        return WA_ERR_INVALID;
    }
    std::lock_guard lock(impl_->mutex);
    if (!context_manager && !impl_->next_handle) {
        return WA_ERR_OVERFLOW;
    }
    const uint32_t id = context_manager ? WA_SERVICEMANAGER_HANDLE : impl_->next_handle;
    try {
        const auto [it, inserted] = impl_->objects.try_emplace(id, binder);
        if (!inserted) {
            return WA_ERR_EXIST;
        }
        uint64_t unset = 0;
        if (!binder->address_.compare_exchange_strong(unset, (uint64_t{pid_} << 32) | id)) {
            impl_->objects.erase(it);
            return WA_ERR_EXIST;
        }
    } catch (const std::bad_alloc&) {
        return WA_ERR_NOMEM;
    }
    if (!context_manager) {
        ++impl_->next_handle;
    }
    return WA_OK;
}

std::shared_ptr<IBinder> BinderProcess::getBinder(BinderHandle handle) {
    if (!handle) {
        return nullptr;
    }
    if (handle.owner_pid != pid_) {
        return std::make_shared<BpBinder>(*this, handle);
    }
    std::lock_guard lock(impl_->mutex);
    const auto it = impl_->objects.find(handle.handle_id);
    return it == impl_->objects.end() ? nullptr : it->second;
}

int32_t BinderProcess::send(BinderHandle target, uint32_t code, const Parcel& data, uint32_t flags,
                            BinderTransaction* transaction) {
    if (current() != this) {
        return WA_ERR_BADF;
    }
    if (!target || (flags & kReply)) {
        return WA_ERR_INVALID;
    }
    if (transaction && transaction->process_) {
        return WA_ERR_BUSY;
    }
    uint32_t id;
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->next_transaction) {
            return WA_ERR_OVERFLOW;
        }
        id = impl_->next_transaction++;
        if (transaction) {
            try {
                impl_->pending.try_emplace(id, target.owner_pid, code);
            } catch (const std::bad_alloc&) {
                return WA_ERR_NOMEM;
            }
        }
    }
    const int32_t status = sendPacket(target.owner_pid, id, code, flags, target.handle_id, data);
    if (transaction) {
        if (status != WA_OK) {
            abandon(id);
        } else {
            transaction->process_ = this;
            transaction->id_ = id;
        }
    }
    return status;
}

void BinderProcess::abandon(uint32_t id) {
    std::lock_guard lock(impl_->mutex);
    impl_->pending.erase(id);
}

int32_t BinderProcess::wait(uint32_t id, Parcel* reply, int32_t timeout_ms) {
    const Deadline deadline(timeout_ms);
    for (;;) {
        {
            std::lock_guard lock(impl_->mutex);
            const auto it = impl_->pending.find(id);
            if (it == impl_->pending.end()) {
                return WA_ERR_BADF;
            }
            auto& pending = it->second;
            if (pending.complete) {
                if (reply && pending.status == WA_OK) {
                    *reply = std::move(pending.reply);
                }
                return pending.status;
            }
        }
        const int64_t remaining = deadline.remaining();
        if (!remaining) {
            return WA_ERR_TIMEDOUT;
        }
        // Another thread may consume our reply while this thread is receiving.
        const int32_t slice =
            remaining < 0 ? kPollIntervalMs
                          : std::min<int64_t>(kPollIntervalMs, (remaining + 999'999) / 1'000'000);
        const int32_t status = pollOnce(slice);
        if (status != WA_OK && status != WA_ERR_TIMEDOUT) {
            return status;
        }
    }
}

int32_t BinderProcess::pollOnce(int32_t timeout_ms) {
    if (current() != this) {
        return WA_ERR_BADF;
    }
    wa_binder_msg_t message{};
    Parcel data;
    uint32_t word = 0;
    int32_t status = WA_OK;
    try {
        std::unique_ptr<uint8_t[]> payload(new uint8_t[WA_BINDER_MAX_PAYLOAD]);
        status = wa_binder_recv(&message, payload.get(), WA_BINDER_MAX_PAYLOAD, timeout_ms);
        if (status != WA_OK) {
            return status;
        }
        if (message.payload_len < kEnvelopeSize || message.payload_len > WA_BINDER_MAX_PAYLOAD) {
            status = WA_ERR_INVALID;
        } else {
            std::memcpy(&word, payload.get(), kEnvelopeSize);
            status =
                data.setData({payload.get() + kEnvelopeSize, message.payload_len - kEnvelopeSize});
        }
    } catch (const std::bad_alloc&) {
        return WA_ERR_NOMEM;
    }
    if (message.flags & kReply) {
        std::lock_guard lock(impl_->mutex);
        const auto it = impl_->pending.find(message.txn_id);
        if (it != impl_->pending.end()) {
            auto& pending = it->second;
            if (!pending.complete && pending.peer == message.src_pid &&
                pending.code == message.code) {
                pending.status = status == WA_OK ? std::bit_cast<int32_t>(word) : status;
                pending.reply = std::move(data);
                pending.complete = true;
            }
        }
        return WA_OK;
    }

    Parcel reply;
    const bool oneway = message.flags & IBinder::FLAG_ONEWAY;
    if (status == WA_OK) {
        const auto target = getBinder({pid_, word});
        status =
            target ? target->transact(message.code, data, &reply, message.flags) : WA_ERR_NOENT;
    }
    if (oneway) {
        return WA_OK;
    }
    if (status != WA_OK || reply.data().size() > WA_BINDER_MAX_PAYLOAD - kEnvelopeSize) {
        if (status == WA_OK) {
            status = WA_ERR_OVERFLOW;
        }
        reply = Parcel{};
    }
    return sendPacket(message.src_pid, message.txn_id, message.code, kReply,
                      std::bit_cast<uint32_t>(status), reply);
}

int32_t BinderProcess::serve(const std::atomic_bool& stop) {
    while (!stop.load(std::memory_order_acquire)) {
        const int32_t status = pollOnce(25);
        if (status != WA_OK && status != WA_ERR_TIMEDOUT) {
            return status;
        }
    }
    return WA_OK;
}

} // namespace wa::binder
