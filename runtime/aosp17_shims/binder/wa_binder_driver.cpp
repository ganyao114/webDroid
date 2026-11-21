#include "wa_binder_driver.h"
#include "wa_binder_syscalls.h"

#include <binder/Binder.h>
#undef B_PACK_CHARS
#include <linux/android/binder.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <span>
#include <thread>
#include <unordered_map>
#include <vector>

namespace wa::aosp_binder {
namespace {

constexpr char kDevice[] = "/dev/binder";
constexpr uint32_t kReply = 1u << 31;

// The ring envelope contains vproc IDs and node IDs. Parcel object offsets and
// pointers in binder_transaction_data are reconstructed in the receiver's memory.
struct WireHeader {
    uint32_t targetNode;
    uint32_t parentTransaction;
    uint32_t dataSize;
    uint32_t objectCount;
    uint32_t senderUid;
};

struct WireObject {
    uint32_t offset;
    Endpoint endpoint;
};

static_assert(std::endian::native == std::endian::little);
static_assert(sizeof(WireHeader) == 20 && sizeof(WireObject) == 12);
static_assert(sizeof(binder_size_t) == 8);

struct Packet {
    wa_binder_msg_t message{};
    std::vector<uint8_t> bytes;
};

struct ReplyRoute {
    wa_pid_t pid;
    uint32_t transaction;
};

struct Thread {
    bool looper = false;
    bool stopping = false;
    std::deque<uint32_t> commands;
    std::deque<Packet> packets;
    std::vector<ReplyRoute> calls;
};

struct Pending {
    wa_pid_t peer;
    uint32_t waiter;
};

thread_local uint32_t threadId;
thread_local bool ringAttached;

template <typename T> T load(const uint8_t* address) {
    T value;
    std::memcpy(&value, address, sizeof(value));
    return value;
}

template <typename T> void store(uint8_t* address, const T& value) {
    std::memcpy(address, &value, sizeof(value));
}

template <typename T> T* addressOf(binder_uintptr_t address) {
    return reinterpret_cast<T*>(static_cast<uintptr_t>(address));
}

bool validEndpoint(Endpoint endpoint) {
    return endpoint.pid > 0 && endpoint.pid <= INT32_MAX && endpoint.node > 0;
}

int fail(int error) {
    errno = error;
    return -1;
}

class Driver {
  public:
    int32_t attach(void* base, size_t bytes, wa_pid_t pid);
    Endpoint publish(const android::sp<android::BBinder>& binder);
    int32_t handleFor(Endpoint endpoint);
    void stopLooper();
    void exitThread();

    void* map(size_t length);
    int unmap(void* address, size_t length);
    int writeRead(binder_write_read& io);

    std::atomic<int> fd{-1};

  private:
    using Lock = std::unique_lock<std::mutex>;

    Thread& currentThread();
    uint32_t publishLocked(android::BBinder* binder);
    int32_t handleForLocked(Endpoint endpoint);
    android::BBinder* findNode(uint32_t node);
    bool encode(const binder_transaction_data& transaction, WireHeader header,
                std::vector<uint8_t>& bytes);
    binder_uintptr_t makeBuffer(std::span<const uint8_t> bytes, const WireHeader& header,
                                binder_transaction_data& transaction);
    bool send(Lock& lock, wa_pid_t dst, uint32_t id, uint32_t code, uint32_t flags,
              const std::vector<uint8_t>& bytes);
    bool sendError(Lock& lock, ReplyRoute route, android::status_t status);
    int write(Lock& lock, Thread& thread, uint32_t command, const uint8_t* data);
    int read(Lock& lock, Thread& thread, binder_write_read& io);
    bool deliver(Lock& lock, Thread& thread, const Packet& packet, binder_write_read& io);
    void route(Packet packet);

    std::mutex mutex_;
    std::condition_variable ready_;
    void* ringBase_ = nullptr;
    size_t ringBytes_ = 0;
    wa_pid_t pid_ = 0;
    bool receiving_ = false;
    uint32_t nextThread_ = 0;
    uint32_t nextTransaction_ = 0;
    std::unordered_map<uint32_t, Thread> threads_;
    std::unordered_map<uint32_t, Pending> pending_;
    std::deque<Packet> requests_;

    std::vector<android::sp<android::BBinder>> nodes_{nullptr};
    std::vector<Endpoint> handles_{{}};
    uint8_t* mapping_ = nullptr;
    size_t mappingSize_ = 0;
    std::map<size_t, size_t> buffers_;
};

Driver& driver() {
    // ProcessState and its published Binder nodes live for the life of the vproc.
    [[clang::no_destroy]] static Driver instance;
    return instance;
}

int32_t Driver::attach(void* base, size_t bytes, wa_pid_t pid) {
    std::lock_guard lock(mutex_);
    if (!validEndpoint({pid, 1}) ||
        (pid_ && (pid_ != pid || ringBase_ != base || ringBytes_ != bytes))) {
        return WA_ERR_INVALID;
    }
    const int32_t result = wa_binder_init_ring(base, bytes, pid);
    if (result == WA_OK) {
        ringBase_ = base;
        ringBytes_ = bytes;
        pid_ = pid;
        ringAttached = true;
    }
    return result;
}

Thread& Driver::currentThread() {
    if (!threadId)
        threadId = ++nextThread_;
    return threads_[threadId];
}

void Driver::stopLooper() {
    std::lock_guard lock(mutex_);
    currentThread().stopping = true;
}

void Driver::exitThread() {
    std::lock_guard lock(mutex_);
    std::erase_if(pending_, [](const auto& entry) { return entry.second.waiter == threadId; });
    threads_.erase(threadId);
    threadId = 0;
}

uint32_t Driver::publishLocked(android::BBinder* binder) {
    const auto found = std::find_if(nodes_.begin(), nodes_.end(),
                                    [binder](const auto& node) { return node.get() == binder; });
    if (found != nodes_.end())
        return found - nodes_.begin();
    nodes_.push_back(android::sp<android::BBinder>::fromExisting(binder));
    return nodes_.size() - 1;
}

Endpoint Driver::publish(const android::sp<android::BBinder>& binder) {
    std::lock_guard lock(mutex_);
    if (!pid_ || !binder)
        return {};
    return {pid_, publishLocked(binder.get())};
}

int32_t Driver::handleForLocked(Endpoint endpoint) {
    if (!validEndpoint(endpoint))
        return -1;
    const auto found = std::find(handles_.begin() + 1, handles_.end(), endpoint);
    if (found != handles_.end())
        return found - handles_.begin();
    if (handles_.size() >= INT32_MAX)
        return -1;
    handles_.push_back(endpoint);
    return handles_.size() - 1;
}

int32_t Driver::handleFor(Endpoint endpoint) {
    std::lock_guard lock(mutex_);
    return handleForLocked(endpoint);
}

android::BBinder* Driver::findNode(uint32_t node) {
    return node < nodes_.size() ? nodes_[node].get() : nullptr;
}

void* Driver::map(size_t length) {
    std::lock_guard lock(mutex_);
    if (mapping_) {
        errno = EBUSY;
        return MAP_FAILED;
    }
    void* address = mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (address != MAP_FAILED) {
        mapping_ = static_cast<uint8_t*>(address);
        mappingSize_ = length;
    }
    return address;
}

int Driver::unmap(void* address, size_t length) {
    std::lock_guard lock(mutex_);
    if (address != mapping_ || length != mappingSize_)
        return munmap(address, length);
    const int result = munmap(address, length);
    if (result == 0) {
        mapping_ = nullptr;
        mappingSize_ = 0;
        buffers_.clear();
    }
    return result;
}

bool Driver::encode(const binder_transaction_data& transaction, WireHeader header,
                    std::vector<uint8_t>& bytes) {
    if (transaction.data_size > WA_BINDER_MAX_PAYLOAD - sizeof(header) ||
        transaction.offsets_size % sizeof(binder_size_t)) {
        return false;
    }
    const auto* data = addressOf<const uint8_t>(transaction.data.ptr.buffer);
    const auto* offsets = addressOf<const binder_size_t>(transaction.data.ptr.offsets);
    const size_t count = transaction.offsets_size / sizeof(binder_size_t);
    if ((!data && transaction.data_size) || (!offsets && count) ||
        count >
            (WA_BINDER_MAX_PAYLOAD - sizeof(header) - transaction.data_size) / sizeof(WireObject)) {
        return false;
    }

    header.dataSize = transaction.data_size;
    header.objectCount = count;
    header.senderUid = geteuid();
    bytes.resize(sizeof(header) + header.dataSize + count * sizeof(WireObject));
    store(bytes.data(), header);
    auto* parcel = bytes.data() + sizeof(header);
    if (header.dataSize)
        std::memcpy(parcel, data, header.dataSize);
    auto* objects = parcel + header.dataSize;

    binder_size_t minimumOffset = 0;
    for (size_t i = 0; i < count; ++i) {
        const binder_size_t offset = offsets[i];
        if (offset < minimumOffset || offset % 4 || offset > header.dataSize ||
            header.dataSize - offset < sizeof(flat_binder_object)) {
            return false;
        }
        const auto flat = load<flat_binder_object>(data + offset);
        Endpoint endpoint{};
        switch (flat.hdr.type) {
        case BINDER_TYPE_BINDER:
            if (flat.binder) {
                auto* binder = addressOf<android::BBinder>(flat.cookie);
                if (!binder || binder->getWeakRefs() != addressOf<void>(flat.binder))
                    return false;
                endpoint = {pid_, publishLocked(binder)};
            }
            break;
        case BINDER_TYPE_HANDLE:
            if (!flat.handle || flat.handle >= handles_.size())
                return false;
            endpoint = handles_[flat.handle];
            break;
        default:
            // Descriptors and scatter-gather buffers require a separate transport.
            return false;
        }
        flat_binder_object wire{};
        wire.hdr.type = endpoint.pid ? BINDER_TYPE_HANDLE : BINDER_TYPE_BINDER;
        wire.flags = flat.flags;
        store(parcel + offset, wire);
        store(objects + i * sizeof(WireObject),
              WireObject{static_cast<uint32_t>(offset), endpoint});
        minimumOffset = offset + sizeof(flat);
    }
    return true;
}

binder_uintptr_t Driver::makeBuffer(std::span<const uint8_t> bytes, const WireHeader& header,
                                    binder_transaction_data& transaction) {
    if (!mapping_)
        return 0;
    constexpr size_t alignment = alignof(binder_size_t);
    const size_t objectStart = (header.dataSize + alignment - 1) & ~(alignment - 1);
    const size_t needed =
        std::max(alignment, objectStart + header.objectCount * sizeof(binder_size_t));
    size_t start = 0;
    for (const auto& [offset, length] : buffers_) {
        if (offset - start >= needed)
            break;
        start = offset + length;
    }
    if (needed > mappingSize_ - start)
        return 0;
    auto* buffer = mapping_ + start;
    auto* offsets = reinterpret_cast<binder_size_t*>(buffer + objectStart);
    std::memcpy(buffer, bytes.data(), header.dataSize);

    size_t minimumOffset = 0;
    for (uint32_t i = 0; i < header.objectCount; ++i) {
        const auto object =
            load<WireObject>(bytes.data() + header.dataSize + i * sizeof(WireObject));
        if (object.offset < minimumOffset || object.offset % 4 || object.offset > header.dataSize ||
            header.dataSize - object.offset < sizeof(flat_binder_object)) {
            return 0;
        }
        const auto wire = load<flat_binder_object>(buffer + object.offset);
        flat_binder_object flat{};
        flat.flags = wire.flags;
        const Endpoint endpoint = object.endpoint;
        if (endpoint == Endpoint{}) {
            flat.hdr.type = BINDER_TYPE_BINDER;
        } else if (endpoint.pid == pid_) {
            auto* binder = findNode(endpoint.node);
            if (!binder)
                return 0;
            flat.hdr.type = BINDER_TYPE_BINDER;
            flat.binder = reinterpret_cast<uintptr_t>(binder->getWeakRefs());
            flat.cookie = reinterpret_cast<uintptr_t>(binder);
        } else {
            const auto handle = handleForLocked(endpoint);
            if (handle < 0)
                return 0;
            flat.hdr.type = BINDER_TYPE_HANDLE;
            flat.handle = handle;
        }
        store(buffer + object.offset, flat);
        offsets[i] = object.offset;
        minimumOffset = object.offset + sizeof(flat);
    }

    buffers_.emplace(start, needed);
    transaction.data_size = header.dataSize;
    transaction.offsets_size = header.objectCount * sizeof(binder_size_t);
    transaction.data.ptr.buffer = reinterpret_cast<uintptr_t>(buffer);
    transaction.data.ptr.offsets = header.objectCount ? reinterpret_cast<uintptr_t>(offsets) : 0;
    return transaction.data.ptr.buffer;
}

bool Driver::send(Lock& lock, wa_pid_t dst, uint32_t id, uint32_t code, uint32_t flags,
                  const std::vector<uint8_t>& bytes) {
    using namespace std::chrono_literals;
    lock.unlock();
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    int32_t result;
    do {
        result = wa_binder_send(dst, id, code, flags, bytes.data(), bytes.size());
        if (result != WA_ERR_AGAIN)
            break;
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    lock.lock();
    return result == WA_OK;
}

bool Driver::sendError(Lock& lock, ReplyRoute route, android::status_t status) {
    std::vector<uint8_t> bytes(sizeof(WireHeader) + sizeof(status));
    store(bytes.data(), WireHeader{0, 0, sizeof(status), 0, geteuid()});
    store(bytes.data() + sizeof(WireHeader), status);
    return send(lock, route.pid, route.transaction, 0, kReply | TF_STATUS_CODE, bytes);
}

int Driver::write(Lock& lock, Thread& thread, uint32_t command, const uint8_t* data) {
    switch (command) {
    case BC_TRANSACTION: {
        const auto transaction = load<binder_transaction_data>(data);
        const auto handle = transaction.target.handle;
        if (!handle || handle >= handles_.size()) {
            thread.commands.push_back(BR_DEAD_REPLY);
            return 0;
        }
        const Endpoint target = handles_[handle];
        const bool synchronous = !(transaction.flags & TF_ONE_WAY);
        uint32_t id;
        do {
            id = ++nextTransaction_;
        } while (!id || pending_.contains(id));
        const uint32_t parent = !thread.calls.empty() && thread.calls.back().pid == target.pid
                                    ? thread.calls.back().transaction
                                    : 0;
        std::vector<uint8_t> bytes;
        if (encode(transaction, WireHeader{target.node, parent, 0, 0, 0}, bytes)) {
            if (synchronous)
                pending_.emplace(id, Pending{target.pid, threadId});
            if (send(lock, target.pid, id, transaction.code, transaction.flags & ~kReply, bytes)) {
                thread.commands.push_back(BR_TRANSACTION_COMPLETE);
                return 0;
            }
            pending_.erase(id);
        }
        thread.commands.push_back(BR_FAILED_REPLY);
        return 0;
    }
    case BC_REPLY: {
        if (thread.calls.empty())
            return fail(EINVAL);
        const ReplyRoute target = thread.calls.back();
        thread.calls.pop_back();
        const auto transaction = load<binder_transaction_data>(data);
        std::vector<uint8_t> bytes;
        const bool sent =
            encode(transaction, {}, bytes)
                ? send(lock, target.pid, target.transaction, 0, kReply | transaction.flags, bytes)
                : sendError(lock, target, android::FAILED_TRANSACTION);
        thread.commands.push_back(sent ? BR_TRANSACTION_COMPLETE : BR_FAILED_REPLY);
        return 0;
    }
    case BC_FREE_BUFFER: {
        const auto address = load<binder_uintptr_t>(data);
        const auto base = reinterpret_cast<uintptr_t>(mapping_);
        if (address < base || !buffers_.erase(address - base))
            return fail(EINVAL);
        return 0;
    }
    case BC_ENTER_LOOPER:
    case BC_REGISTER_LOOPER:
        thread.looper = true;
        return 0;
    case BC_EXIT_LOOPER:
        thread.looper = false;
        return 0;
    case BC_INCREFS:
    case BC_ACQUIRE:
    case BC_RELEASE:
    case BC_DECREFS:
        // The publisher pins nodes for the vproc's lifetime.
        return 0;
    default:
        return fail(ENOTTY);
    }
}

bool Driver::deliver(Lock& lock, Thread& thread, const Packet& packet, binder_write_read& io) {
    const auto& message = packet.message;
    const bool reply = message.flags & kReply;
    const auto header = load<WireHeader>(packet.bytes.data());
    uint32_t command = reply ? BR_REPLY : BR_TRANSACTION;
    binder_transaction_data transaction{};
    transaction.code = message.code;
    transaction.flags = message.flags & ~kReply;
    transaction.sender_pid = message.src_pid;
    transaction.sender_euid = header.senderUid;

    if (!reply) {
        auto* binder = findNode(header.targetNode);
        if (!binder) {
            if (!(transaction.flags & TF_ONE_WAY)) {
                sendError(lock, {message.src_pid, message.txn_id}, android::DEAD_OBJECT);
            }
            return false;
        }
        transaction.target.ptr = reinterpret_cast<uintptr_t>(binder->getWeakRefs());
        transaction.cookie = reinterpret_cast<uintptr_t>(binder);
    }

    if (!makeBuffer(std::span(packet.bytes).subspan(sizeof(header)), header, transaction)) {
        if (!reply) {
            if (!(transaction.flags & TF_ONE_WAY)) {
                sendError(lock, {message.src_pid, message.txn_id}, android::FAILED_TRANSACTION);
            }
            return false;
        }
        command = BR_FAILED_REPLY;
    } else if (!reply && !(transaction.flags & TF_ONE_WAY)) {
        thread.calls.push_back({message.src_pid, message.txn_id});
    }

    auto* output = addressOf<uint8_t>(io.read_buffer);
    store(output, command);
    io.read_consumed = sizeof(command);
    if (command != BR_FAILED_REPLY) {
        store(output + sizeof(command), transaction);
        io.read_consumed += sizeof(transaction);
    }
    return true;
}

void Driver::route(Packet packet) {
    const auto& message = packet.message;
    if (message.dst_pid != pid_ || !validEndpoint({message.src_pid, 1}) || !message.txn_id ||
        message.payload_len < sizeof(WireHeader)) {
        return;
    }
    const auto header = load<WireHeader>(packet.bytes.data());
    if (sizeof(header) + uint64_t{header.dataSize} +
            uint64_t{header.objectCount} * sizeof(WireObject) !=
        message.payload_len) {
        return;
    }
    const bool reply = message.flags & kReply;
    if (message.flags & TF_STATUS_CODE &&
        (!reply || header.dataSize != sizeof(android::status_t) || header.objectCount)) {
        return;
    }
    const uint32_t directed = reply ? message.txn_id : header.parentTransaction;
    if (directed) {
        const auto call = pending_.find(directed);
        if (call != pending_.end() && call->second.peer == message.src_pid) {
            threads_.at(call->second.waiter).packets.push_back(std::move(packet));
            if (reply)
                pending_.erase(call);
            return;
        }
    }
    if (!reply)
        requests_.push_back(std::move(packet));
}

int Driver::read(Lock& lock, Thread& thread, binder_write_read& io) {
    for (;;) {
        if (!thread.commands.empty()) {
            store(addressOf<uint8_t>(io.read_buffer), thread.commands.front());
            thread.commands.pop_front();
            io.read_consumed = sizeof(uint32_t);
            return 0;
        }
        if (thread.stopping && thread.calls.empty()) {
            auto* output = addressOf<uint8_t>(io.read_buffer);
            store(output, uint32_t{BR_ERROR});
            store(output + sizeof(uint32_t), int32_t{-ECONNREFUSED});
            io.read_consumed = 2 * sizeof(uint32_t);
            return 0;
        }
        if (!thread.packets.empty() || (thread.looper && !requests_.empty())) {
            auto& queue = thread.packets.empty() ? requests_ : thread.packets;
            Packet packet = std::move(queue.front());
            queue.pop_front();
            if (deliver(lock, thread, packet, io))
                return 0;
            continue;
        }
        if (receiving_) {
            ready_.wait(lock);
            continue;
        }

        // All threads of a vproc share a destination PID in M0. One reader at
        // a time drains that queue and wakes the appropriate Binder thread.
        receiving_ = true;
        lock.unlock();
        Packet packet;
        packet.bytes.resize(WA_BINDER_MAX_PAYLOAD);
        const auto result =
            wa_binder_recv(&packet.message, packet.bytes.data(), packet.bytes.size(), 50);
        lock.lock();
        receiving_ = false;
        if (result == WA_OK && packet.message.payload_len <= packet.bytes.size()) {
            packet.bytes.resize(packet.message.payload_len);
            route(std::move(packet));
        }
        ready_.notify_all();
        if (result != WA_OK && result != WA_ERR_TIMEDOUT && result != WA_ERR_AGAIN) {
            return fail(ECONNREFUSED);
        }
    }
}

int Driver::writeRead(binder_write_read& io) {
    if ((io.write_size && !io.write_buffer) ||
        (io.read_size &&
         (!io.read_buffer || io.read_size < sizeof(uint32_t) + sizeof(binder_transaction_data)))) {
        return fail(EINVAL);
    }
    Lock lock(mutex_);
    if (!pid_ || !mapping_)
        return fail(EBADF);
    if (!ringAttached) {
        if (wa_binder_init_ring(ringBase_, ringBytes_, pid_) != WA_OK)
            return fail(ECONNREFUSED);
        ringAttached = true;
    }
    auto& thread = currentThread();
    const auto* input = addressOf<const uint8_t>(io.write_buffer);
    io.read_consumed = io.write_consumed = 0;
    while (io.write_consumed < io.write_size) {
        const auto remaining = io.write_size - io.write_consumed;
        if (remaining < sizeof(uint32_t))
            return fail(EINVAL);
        const auto* command = input + io.write_consumed;
        const uint32_t code = load<uint32_t>(command);
        const size_t size = sizeof(code) + _IOC_SIZE(code);
        if (remaining < size)
            return fail(EINVAL);
        if (write(lock, thread, code, command + sizeof(code)) < 0)
            return -1;
        io.write_consumed += size;
    }
    return io.read_size ? read(lock, thread, io) : 0;
}

} // namespace

int32_t attach(void* base, size_t bytes, wa_pid_t pid) {
    return driver().attach(base, bytes, pid);
}

Endpoint publish(const android::sp<android::BBinder>& binder) {
    return driver().publish(binder);
}

int32_t handleFor(Endpoint endpoint) {
    return driver().handleFor(endpoint);
}

void stopLooper() {
    driver().stopLooper();
}

} // namespace wa::aosp_binder

using wa::aosp_binder::driver;
using wa::aosp_binder::fail;
using wa::aosp_binder::kDevice;

int wa_binder_open(const char* path, int flags) {
    if (std::strcmp(path, kDevice))
        return open(path, flags);
    const int fd = open("/dev/null", flags);
    if (fd < 0)
        return fd;
    int unopened = -1;
    if (driver().fd.compare_exchange_strong(unopened, fd))
        return fd;
    close(fd);
    return fail(EBUSY);
}

int wa_binder_access(const char* path, int mode) {
    return std::strcmp(path, kDevice) ? access(path, mode) : 0;
}

int wa_binder_close(int fd) {
    int expected = fd;
    driver().fd.compare_exchange_strong(expected, -1);
    return close(fd);
}

void* wa_binder_mmap(void* address, size_t length, int protection, int flags, int fd,
                     off_t offset) {
    if (fd >= 0 && fd == driver().fd)
        return driver().map(length);
    return mmap(address, length, protection, flags, fd, offset);
}

int wa_binder_munmap(void* address, size_t length) {
    return driver().unmap(address, length);
}

int wa_binder_ioctl(int fd, unsigned long request, void* argument) {
    if (fd < 0 || fd != driver().fd)
        return fail(EBADF);
    switch (request) {
    case BINDER_VERSION:
        if (!argument)
            return fail(EFAULT);
        static_cast<binder_version*>(argument)->protocol_version = BINDER_CURRENT_PROTOCOL_VERSION;
        return 0;
    case BINDER_SET_MAX_THREADS:
        // ProcessState manages the configured limit. This device uses explicit loopers.
        return argument ? 0 : fail(EFAULT);
    case BINDER_WRITE_READ:
        return argument ? driver().writeRead(*static_cast<binder_write_read*>(argument))
                        : fail(EFAULT);
    case BINDER_THREAD_EXIT:
        driver().exitThread();
        return 0;
    default:
        return fail(ENOTTY);
    }
}
