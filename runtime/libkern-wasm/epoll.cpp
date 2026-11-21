#include "kernel_internal.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>

namespace {

constexpr int32_t kFirstFd = 0x4000'0000;
constexpr uint32_t kSemaphore = 1;
constexpr uint64_t kMaxCounter = UINT64_MAX - 1;
constexpr uint32_t kEvents = WA_EPOLLIN | WA_EPOLLOUT | WA_EPOLLERR | WA_EPOLLHUP;

enum class Kind : uint8_t { Free, Event, PipeRead, PipeWrite, Epoll };

struct Descriptor {
    Kind kind;
    uint32_t flags;
    uint32_t object;
    uint32_t generation;
    uint64_t counter;
};

struct Pipe {
    bool read_open;
    bool write_open;
    size_t head;
    size_t size;
    std::array<uint8_t, 4096> bytes;
};

struct Watch {
    int32_t fd;
    wa_epoll_event_t event;
};

struct Epoll {
    bool used;
    size_t cursor;
    std::array<Watch, 128> watches;
};

constinit wa::FutexMutex descriptor_lock;
constinit uint32_t epoch;
constinit std::array<Descriptor, 256> descriptors{};
constinit std::array<Pipe, 32> pipes{};
constinit std::array<Epoll, 16> epolls{};

Descriptor* find_fd(int32_t fd) {
    if (!wa::is_event_fd(fd)) {
        return nullptr;
    }
    auto& descriptor = descriptors[fd - kFirstFd];
    return descriptor.kind == Kind::Free ? nullptr : &descriptor;
}

int32_t allocate_fd(Kind kind, uint32_t flags, uint32_t object = 0) {
    for (size_t i = 0; i < descriptors.size(); ++i) {
        auto& descriptor = descriptors[i];
        if (descriptor.kind == Kind::Free) {
            descriptor = {kind, flags, object, descriptor.generation + 1, 0};
            return kFirstFd + static_cast<int32_t>(i);
        }
    }
    return -24;  // EMFILE
}

void signal_change() {
    wa::atomic(epoch).fetch_add(1, std::memory_order_release);
    wa_futex_wake(&epoch, UINT32_MAX);
}

uint32_t ready_events(const Descriptor& descriptor) {
    if (descriptor.kind == Kind::Event) {
        return (descriptor.counter ? WA_EPOLLIN : 0) |
               (descriptor.counter < kMaxCounter ? WA_EPOLLOUT : 0);
    }
    const Pipe& pipe = pipes[descriptor.object];
    if (descriptor.kind == Kind::PipeRead) {
        return (pipe.size ? WA_EPOLLIN : 0) | (!pipe.write_open ? WA_EPOLLHUP : 0);
    }
    return (pipe.size < pipe.bytes.size() ? WA_EPOLLOUT : 0) |
           (!pipe.read_open ? WA_EPOLLERR : 0);
}

template <typename Transfer>
int64_t transfer_event(int32_t fd, Transfer transfer) {
    std::unique_lock lock(descriptor_lock);
    Descriptor* descriptor = find_fd(fd);
    if (!descriptor) {
        return WA_ERR_BADF;
    }
    const uint32_t generation = descriptor->generation;
    for (;;) {
        const int64_t result = transfer(*descriptor);
        if (result != WA_ERR_AGAIN || descriptor->flags & wa::kNonblock) {
            return result;
        }
        const uint32_t observed = wa::atomic(epoch).load(std::memory_order_relaxed);
        lock.unlock();
        wa_futex_wait(&epoch, observed, -1);
        lock.lock();
        if (descriptor->kind == Kind::Free || descriptor->generation != generation) {
            return WA_ERR_BADF;
        }
    }
}

}  // namespace

namespace wa {

bool is_event_fd(int32_t fd) {
    return fd >= kFirstFd && fd < kFirstFd + static_cast<int32_t>(descriptors.size());
}

int32_t pipe2(int32_t pipefd[2], uint32_t flags) {
    if (!pipefd) {
        return WA_ERR_FAULT;
    }
    if (flags & ~(kCloexec | kNonblock)) {
        return WA_ERR_INVALID;
    }

    std::lock_guard lock(descriptor_lock);
    for (uint32_t i = 0; i < pipes.size(); ++i) {
        Pipe& pipe = pipes[i];
        if (pipe.read_open || pipe.write_open) {
            continue;
        }
        const int32_t reader = allocate_fd(Kind::PipeRead, flags, i);
        if (reader < 0) {
            return reader;
        }
        const int32_t writer = allocate_fd(Kind::PipeWrite, flags, i);
        if (writer < 0) {
            find_fd(reader)->kind = Kind::Free;
            return writer;
        }
        pipe.read_open = pipe.write_open = true;
        pipe.head = pipe.size = 0;
        pipefd[0] = reader;
        pipefd[1] = writer;
        return WA_OK;
    }
    return -24;  // EMFILE
}

int64_t event_read(int32_t fd, void* buf, size_t count) {
    return transfer_event(fd, [&](Descriptor& descriptor) -> int64_t {
        if (descriptor.kind == Kind::Event) {
            if (count < sizeof(uint64_t)) {
                return WA_ERR_INVALID;
            }
            if (!descriptor.counter) {
                return WA_ERR_AGAIN;
            }
            const uint64_t value = descriptor.flags & kSemaphore ? 1 : descriptor.counter;
            std::memcpy(buf, &value, sizeof(value));
            descriptor.counter -= value;
            signal_change();
            return sizeof(value);
        }
        if (descriptor.kind != Kind::PipeRead) {
            return WA_ERR_BADF;
        }
        Pipe& pipe = pipes[descriptor.object];
        if (count == 0 || (!pipe.size && !pipe.write_open)) {
            return 0;
        }
        if (!pipe.size) {
            return WA_ERR_AGAIN;
        }
        const size_t len = std::min(count, pipe.size);
        const size_t first = std::min(len, pipe.bytes.size() - pipe.head);
        std::memcpy(buf, pipe.bytes.data() + pipe.head, first);
        std::memcpy(static_cast<uint8_t*>(buf) + first, pipe.bytes.data(), len - first);
        pipe.head = (pipe.head + len) % pipe.bytes.size();
        pipe.size -= len;
        signal_change();
        return len;
    });
}

int64_t event_write(int32_t fd, const void* buf, size_t count) {
    size_t written = 0;
    const int64_t result = transfer_event(fd, [&](Descriptor& descriptor) -> int64_t {
        if (descriptor.kind == Kind::Event) {
            if (count < sizeof(uint64_t)) {
                return WA_ERR_INVALID;
            }
            uint64_t value;
            std::memcpy(&value, buf, sizeof(value));
            if (value > kMaxCounter) {
                return WA_ERR_INVALID;
            }
            if (descriptor.counter > kMaxCounter - value) {
                return WA_ERR_AGAIN;
            }
            descriptor.counter += value;
            if (value) {
                signal_change();
            }
            return sizeof(value);
        }
        if (descriptor.kind != Kind::PipeWrite) {
            return WA_ERR_BADF;
        }
        if (!count) {
            return 0;
        }
        Pipe& pipe = pipes[descriptor.object];
        if (!pipe.read_open) {
            return -32;  // EPIPE
        }
        const size_t space = pipe.bytes.size() - pipe.size;
        if (!space || (count <= pipe.bytes.size() && space < count)) {
            return WA_ERR_AGAIN;
        }
        const size_t len = std::min(count - written, space);
        const size_t tail = (pipe.head + pipe.size) % pipe.bytes.size();
        const size_t first = std::min(len, pipe.bytes.size() - tail);
        const auto* source = static_cast<const uint8_t*>(buf) + written;
        std::memcpy(pipe.bytes.data() + tail, source, first);
        std::memcpy(pipe.bytes.data(), source + first, len - first);
        pipe.size += len;
        written += len;
        signal_change();
        if (written == count || descriptor.flags & kNonblock) {
            return written;
        }
        return WA_ERR_AGAIN;
    });
    return result < 0 && written ? static_cast<int64_t>(written) : result;
}

int32_t event_close(int32_t fd) {
    std::lock_guard lock(descriptor_lock);
    Descriptor* descriptor = find_fd(fd);
    if (!descriptor) {
        return WA_ERR_BADF;
    }
    switch (descriptor->kind) {
    case Kind::PipeRead:
        pipes[descriptor->object].read_open = false;
        break;
    case Kind::PipeWrite:
        pipes[descriptor->object].write_open = false;
        break;
    case Kind::Epoll:
        epolls[descriptor->object].used = false;
        break;
    default:
        break;
    }
    descriptor->kind = Kind::Free;
    for (Epoll& epoll : epolls) {
        if (epoll.used) {
            for (Watch& watch : epoll.watches) {
                if (watch.fd == fd) {
                    watch = {};
                }
            }
        }
    }
    signal_change();
    return WA_OK;
}

}  // namespace wa

extern "C" int32_t wa_eventfd(uint64_t initval, uint32_t flags) {
    if (initval > kMaxCounter || flags & ~(kSemaphore | wa::kNonblock | wa::kCloexec)) {
        return WA_ERR_INVALID;
    }
    std::lock_guard lock(descriptor_lock);
    const int32_t fd = allocate_fd(Kind::Event, flags);
    if (fd >= 0) {
        find_fd(fd)->counter = initval;
    }
    return fd;
}

extern "C" int32_t wa_pipe(int32_t pipefd[2]) {
    return wa::pipe2(pipefd, 0);
}

extern "C" int32_t wa_epoll_create(uint32_t flags) {
    if (flags & ~wa::kCloexec) {
        return WA_ERR_INVALID;
    }
    std::lock_guard lock(descriptor_lock);
    for (uint32_t i = 0; i < epolls.size(); ++i) {
        if (!epolls[i].used) {
            const int32_t fd = allocate_fd(Kind::Epoll, flags, i);
            if (fd >= 0) {
                epolls[i] = {};
                epolls[i].used = true;
            }
            return fd;
        }
    }
    return -24;  // EMFILE
}

extern "C" int32_t wa_epoll_ctl(int32_t epfd, int32_t op, int32_t fd,
                                 const wa_epoll_event_t* event) {
    if (op != WA_EPOLL_CTL_ADD && op != WA_EPOLL_CTL_DEL && op != WA_EPOLL_CTL_MOD) {
        return WA_ERR_INVALID;
    }
    if (op != WA_EPOLL_CTL_DEL) {
        if (!event) {
            return WA_ERR_FAULT;
        }
        if (event->events & ~kEvents) {
            return WA_ERR_INVALID;
        }
    }
    std::lock_guard lock(descriptor_lock);
    Descriptor* ep = find_fd(epfd);
    Descriptor* target = find_fd(fd);
    if (!ep || !target) {
        return WA_ERR_BADF;
    }
    if (ep->kind != Kind::Epoll || epfd == fd) {
        return WA_ERR_INVALID;
    }
    if (target->kind == Kind::Epoll) {
        return WA_ERR_NOTSUP;
    }
    Watch* free_watch = nullptr;
    for (Watch& watch : epolls[ep->object].watches) {
        if (watch.fd == fd) {
            if (op == WA_EPOLL_CTL_ADD) {
                return WA_ERR_EXIST;
            }
            watch = op == WA_EPOLL_CTL_DEL ? Watch{} : Watch{fd, *event};
            signal_change();
            return WA_OK;
        }
        if (!watch.fd && !free_watch) {
            free_watch = &watch;
        }
    }
    if (op != WA_EPOLL_CTL_ADD) {
        return WA_ERR_NOENT;
    }
    if (!free_watch) {
        return -28;  // ENOSPC
    }
    *free_watch = {fd, *event};
    signal_change();
    return WA_OK;
}

extern "C" int32_t wa_epoll_wait(int32_t epfd, wa_epoll_event_t* events, int32_t maxevents,
                                  int32_t timeout_ms) {
    if (!events) {
        return WA_ERR_FAULT;
    }
    if (maxevents <= 0) {
        return WA_ERR_INVALID;
    }
    const wa::Deadline deadline(timeout_ms);
    std::unique_lock lock(descriptor_lock);
    Descriptor* descriptor = find_fd(epfd);
    if (!descriptor) {
        return WA_ERR_BADF;
    }
    if (descriptor->kind != Kind::Epoll) {
        return WA_ERR_INVALID;
    }
    Epoll& epoll = epolls[descriptor->object];
    const uint32_t generation = descriptor->generation;
    for (;;) {
        int32_t count = 0;
        size_t index = epoll.cursor;
        for (size_t i = 0; i < epoll.watches.size() && count < maxevents; ++i) {
            const Watch& watch = epoll.watches[index];
            index = (index + 1) % epoll.watches.size();
            if (watch.fd) {
                const uint32_t mask = ready_events(*find_fd(watch.fd)) &
                                      (watch.event.events | WA_EPOLLERR | WA_EPOLLHUP);
                if (mask) {
                    events[count++] = {mask, 0, watch.event.data};
                    epoll.cursor = index;
                }
            }
        }
        if (count) {
            return count;
        }
        const int64_t remaining = deadline.remaining();
        if (!remaining) {
            return 0;
        }
        const uint32_t observed = wa::atomic(epoch).load(std::memory_order_relaxed);
        lock.unlock();
        wa_futex_wait(&epoch, observed, remaining);
        lock.lock();
        if (descriptor->kind != Kind::Epoll || descriptor->generation != generation) {
            return WA_ERR_BADF;
        }
    }
}
