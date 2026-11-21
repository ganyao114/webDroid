#include "kernel_internal.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <vector>

#if !defined(__EMSCRIPTEN__)
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/random.h>
#endif
#endif

#if defined(__EMSCRIPTEN__)
extern "C" int __wrap_sched_yield() {
    uint32_t pause = 0;
    wa_futex_wait(&pause, 0, 1'000);
    return 0;
}
#endif

namespace wa::posix {
namespace {

constexpr int32_t kAtCwd = -100;
constexpr int32_t kAtSymlinkNoFollow = 0x100;
constexpr int32_t kAtNoAutomount = 0x800;
constexpr int32_t kAtEmptyPath = 0x1000;
constexpr int32_t kEsPipe = -29;
constexpr int32_t kEmFile = -24;
constexpr int32_t kMaxIov = 1024;
constexpr size_t kMaxTransfer = INT32_MAX & ~(WA_PAGE_SIZE - 1u);
constexpr uint32_t kStatusFlags = kNonblock | WA_O_APPEND;

#if !defined(__EMSCRIPTEN__)
int32_t linuxError(int error) {
    switch (error) {
    case EAGAIN:
        return WA_ERR_AGAIN;
    case EOVERFLOW:
        return WA_ERR_OVERFLOW;
    case ENOSYS:
        return WA_ERR_NOSYS;
    case ENOTSUP:
        return WA_ERR_NOTSUP;
#if EOPNOTSUPP != ENOTSUP
    case EOPNOTSUPP:
        return WA_ERR_NOTSUP;
#endif
    case ENAMETOOLONG:
        return -36;
    case ELOOP:
        return -40;
    default:
        return -error;
    }
}

template <typename T> T nativeResult(T result) {
    return result < 0 ? linuxError(errno) : result;
}
#endif

enum class Kind { File, Pipe, Event, Epoll };

struct OpenFile;
int32_t closeBacking(const OpenFile& file);

// The syscall table gives dup'd descriptors independent FD_CLOEXEC bits while
// retaining a single open file description. Native filesystem descriptors are
// already managed by the host kernel. OPFS uses positional I/O to share a cursor.
struct OpenFile {
    OpenFile(int32_t raw_fd, Kind type, uint32_t status_flags)
        : raw(raw_fd), kind(type), flags(status_flags) {}
    ~OpenFile() {
        if (close_on_release) {
            closeBacking(*this);
        }
    }

    const int32_t raw;
    const Kind kind;
    std::atomic<uint32_t> flags;
    size_t descriptors = 1; // Protected by descriptor_mutex.
    bool close_on_release = false;
#if defined(__EMSCRIPTEN__)
    FutexMutex cursor_mutex;
    int64_t position = 0;
#endif
};

struct Descriptor {
    std::shared_ptr<OpenFile> file;
    int32_t flags;
};

constinit FutexMutex descriptor_mutex;
constinit uint32_t pipe_epoch = 0;
constinit std::optional<std::map<int32_t, Descriptor>> descriptor_table;

using ThreadName = std::array<char, 16>;
#if defined(__EMSCRIPTEN__)
constinit FutexMutex thread_names_mutex;
constinit std::optional<std::map<uint64_t, ThreadName>> thread_names;
#endif

auto& descriptors() {
    if (!descriptor_table) {
        descriptor_table.emplace();
    }
    return *descriptor_table;
}

void signalPipe() {
    atomic(pipe_epoch).fetch_add(1, std::memory_order_release);
    wa_futex_wake(&pipe_epoch, UINT32_MAX);
}

int32_t closeBacking(const OpenFile& file) {
    const int32_t status = wa_vfs_close(file.raw);
    if (file.kind != Kind::File) {
        std::lock_guard lock(descriptor_mutex);
        const auto it = descriptors().find(file.raw);
        if (it != descriptors().end() && !it->second.file) {
            descriptors().erase(it);
        }
        signalPipe();
    }
    return status;
}

#if defined(__EMSCRIPTEN__)
int32_t freeDescriptorLocked(int32_t minimum) {
    int32_t fd = std::max(3, minimum);
    for (;;) {
        if (!is_event_fd(fd) && !descriptors().contains(fd)) {
            return fd;
        }
        if (fd == INT32_MAX) {
            return kEmFile;
        }
        ++fd;
    }
}
#endif

std::shared_ptr<OpenFile> lookupFile(int32_t& fd) {
    std::lock_guard lock(descriptor_mutex);
    const auto it = descriptors().find(fd);
    if (it != descriptors().end()) {
        if (!it->second.file) {
            fd = -1;
        }
        return it->second.file;
    }
#if defined(__EMSCRIPTEN__)
    if (fd > 2 && !is_event_fd(fd)) {
        fd = -1;
    }
#endif
    return nullptr;
}

int32_t remember(int32_t raw, Kind type, uint32_t flags) {
    if (raw < 0) {
        return raw;
    }
    int32_t fd = raw;
    try {
        auto file = std::make_shared<OpenFile>(raw, type, flags & (3 | kStatusFlags));
        std::lock_guard lock(descriptor_mutex);
#if defined(__EMSCRIPTEN__)
        if (type == Kind::File) {
            fd = freeDescriptorLocked(3);
        }
#endif
        if (fd >= 0) {
            descriptors().insert_or_assign(fd,
                                           Descriptor{std::move(file), (flags & kCloexec) ? 1 : 0});
            return fd;
        }
    } catch (const std::bad_alloc&) {
        fd = WA_ERR_NOMEM;
    }
    wa_vfs_close(raw);
    return fd;
}

int64_t transfer(int32_t fd, const std::shared_ptr<OpenFile>& file, void* buffer, size_t count,
                 bool write) {
    const int32_t raw = file ? file->raw : fd;
    if ((!buffer && count) || count > static_cast<size_t>(INT64_MAX)) {
        return WA_ERR_FAULT;
    }
    auto io = [&](void* data, size_t length) {
        return write ? wa_vfs_write(raw, data, length) : wa_vfs_read(raw, data, length);
    };
    if (!file || file->kind == Kind::Epoll) {
        return io(buffer, count);
    }
#if defined(__EMSCRIPTEN__)
    if (file->kind == Kind::File) {
        std::lock_guard lock(file->cursor_mutex);
        int64_t position = file->position;
        if (write && (file->flags.load(std::memory_order_relaxed) & WA_O_APPEND)) {
            wa_stat_t st;
            const int32_t status = wa_vfs_fstat(raw, &st);
            if (status != WA_OK) {
                return status;
            }
            if (st.size > INT64_MAX) {
                return WA_ERR_OVERFLOW;
            }
            position = static_cast<int64_t>(st.size);
        }
        if (count > static_cast<uint64_t>(INT64_MAX - position)) {
            return WA_ERR_OVERFLOW;
        }
        const int64_t done = write ? wa_vfs_pwrite(raw, buffer, count, position)
                                   : wa_vfs_pread(raw, buffer, count, position);
        if (done > 0) {
            file->position = position + done;
        }
        return done;
    }
#endif
    size_t done = 0;
    for (;;) {
        const uint32_t observed = atomic(pipe_epoch).load(std::memory_order_acquire);
        auto* data = static_cast<uint8_t*>(buffer);
        const int64_t result = io(done ? data + done : data, count - done);
        if (result > 0) {
            signalPipe();
        }
        const bool nonblock = file->flags.load(std::memory_order_relaxed) & kNonblock;
        if (result != WA_ERR_AGAIN || nonblock) {
            if (result <= 0) {
                return done ? static_cast<int64_t>(done) : result;
            }
            done += static_cast<size_t>(result);
            if (!write || file->kind != Kind::Pipe || done == count || nonblock) {
                return done;
            }
            continue;
        }
        // The original wa_vfs_* API can also signal a pipe. Its independent
        // wait queue is checked again even if it didn't advance this epoch.
        wa_futex_wait(&pipe_epoch, observed, 10'000'000);
    }
}

#if !defined(__EMSCRIPTEN__)
int64_t nativeFcntl(int32_t fd, int32_t command, int32_t argument) {
    switch (command) {
    case 0:
        return nativeResult(::fcntl(fd, F_DUPFD, argument));
    case 1:
        return nativeResult(::fcntl(fd, F_GETFD));
    case 2:
        return nativeResult(::fcntl(fd, F_SETFD, argument & 1 ? FD_CLOEXEC : 0));
    case 3: {
        const int32_t flags = nativeResult(::fcntl(fd, F_GETFL));
        if (flags < 0) {
            return flags;
        }
        return (flags & O_ACCMODE) | (flags & O_APPEND ? WA_O_APPEND : 0) |
               (flags & O_NONBLOCK ? kNonblock : 0);
    }
    case 4: {
        const int32_t flags = nativeResult(::fcntl(fd, F_GETFL));
        return flags < 0 ? flags
                         : nativeResult(::fcntl(fd, F_SETFL,
                                                (flags & ~(O_APPEND | O_NONBLOCK)) |
                                                    (argument & WA_O_APPEND ? O_APPEND : 0) |
                                                    (argument & kNonblock ? O_NONBLOCK : 0)));
    }
    case 1030:
        return nativeResult(::fcntl(fd, F_DUPFD_CLOEXEC, argument));
    default:
        return WA_ERR_INVALID;
    }
}
#endif

int32_t duplicateLocked(const Descriptor& original, int32_t minimum, bool cloexec) {
    if (minimum < 0) {
        return WA_ERR_INVALID;
    }
#if defined(__EMSCRIPTEN__)
    const int32_t fd = freeDescriptorLocked(minimum);
#else
    // Reserve a real fd so future native opens cannot alias a virtual dup.
    int32_t fd = nativeResult(::open("/dev/null", O_RDONLY | O_CLOEXEC));
    if (fd >= 0 && fd < minimum) {
        const int32_t placeholder = fd;
        fd = nativeResult(::fcntl(placeholder, F_DUPFD_CLOEXEC, minimum));
        ::close(placeholder);
    }
#endif
    if (fd < 0) {
        return fd;
    }
    try {
        descriptors().emplace(fd, Descriptor{original.file, cloexec ? 1 : 0});
    } catch (const std::bad_alloc&) {
#if !defined(__EMSCRIPTEN__)
        ::close(fd);
#endif
        return WA_ERR_NOMEM;
    }
    ++original.file->descriptors;
    return fd;
}

struct Time {
    int64_t seconds = 0;
    uint32_t nanos = 0;
};

struct FileStat {
    uint64_t device = 0;
    uint64_t inode = 0;
    uint32_t mode = 0;
    uint32_t links = 1;
    uint32_t uid = 0;
    uint32_t gid = 0;
    uint64_t special_device = 0;
    int64_t size = 0;
    int32_t block_size = WA_PAGE_SIZE;
    int64_t blocks = 0;
    Time access, modification, change;
};

// Linux UAPI layouts. Explicit padding keeps the ARM EABI layout on wasm32.
struct Stat32 {
    uint32_t device, inode;
    uint16_t mode, links, uid, gid;
    uint32_t special_device, size, block_size, blocks;
    uint32_t access_sec, access_ns, modify_sec, modify_ns, change_sec, change_ns;
    uint32_t unused[2];
};

struct Stat64Arm {
    uint64_t device;
    uint32_t pad0, inode_low, mode, links, uid, gid;
    uint64_t special_device;
    uint64_t pad1;
    int64_t size;
    uint32_t block_size, pad2;
    int64_t blocks;
    uint32_t access_sec, access_ns, modify_sec, modify_ns, change_sec, change_ns;
    uint64_t inode;
};

struct Stat64Generic {
    uint64_t device, inode;
    uint32_t mode, links, uid, gid;
    uint64_t special_device, pad1;
    int64_t size;
    int32_t block_size, pad2;
    int64_t blocks;
    int64_t access_sec;
    uint64_t access_ns;
    int64_t modify_sec;
    uint64_t modify_ns;
    int64_t change_sec;
    uint64_t change_ns;
    uint32_t unused[2];
};

static_assert(sizeof(Stat32) == 64 && offsetof(Stat32, size) == 20);
static_assert(sizeof(Stat64Arm) == 104 && offsetof(Stat64Arm, size) == 48 &&
              offsetof(Stat64Arm, inode) == 96);
static_assert(sizeof(Stat64Generic) == 128 && offsetof(Stat64Generic, size) == 48);

int32_t storeStat(const FileStat& st, void* out, bool arm32, bool wide) {
    const auto access = static_cast<uint32_t>(st.access.seconds);
    const auto modified = static_cast<uint32_t>(st.modification.seconds);
    const auto changed = static_cast<uint32_t>(st.change.seconds);
    if (!arm32) {
        const Stat64Generic value{st.device,
                                  st.inode,
                                  st.mode,
                                  st.links,
                                  st.uid,
                                  st.gid,
                                  st.special_device,
                                  0,
                                  st.size,
                                  st.block_size,
                                  0,
                                  st.blocks,
                                  st.access.seconds,
                                  st.access.nanos,
                                  st.modification.seconds,
                                  st.modification.nanos,
                                  st.change.seconds,
                                  st.change.nanos,
                                  {}};
        std::memcpy(out, &value, sizeof(value));
    } else if (wide) {
        const Stat64Arm value{st.device,
                              0,
                              static_cast<uint32_t>(st.inode),
                              st.mode,
                              st.links,
                              st.uid,
                              st.gid,
                              st.special_device,
                              0,
                              st.size,
                              static_cast<uint32_t>(st.block_size),
                              0,
                              st.blocks,
                              access,
                              st.access.nanos,
                              modified,
                              st.modification.nanos,
                              changed,
                              st.change.nanos,
                              st.inode};
        std::memcpy(out, &value, sizeof(value));
    } else {
        if (st.inode > UINT32_MAX || st.device > UINT32_MAX || st.special_device > UINT32_MAX ||
            st.links > UINT16_MAX || st.size > INT32_MAX || st.blocks > UINT32_MAX) {
            return WA_ERR_OVERFLOW;
        }
        const Stat32 value{static_cast<uint32_t>(st.device),
                           static_cast<uint32_t>(st.inode),
                           static_cast<uint16_t>(st.mode),
                           static_cast<uint16_t>(st.links),
                           static_cast<uint16_t>(st.uid > UINT16_MAX ? 65534 : st.uid),
                           static_cast<uint16_t>(st.gid > UINT16_MAX ? 65534 : st.gid),
                           static_cast<uint32_t>(st.special_device),
                           static_cast<uint32_t>(st.size),
                           static_cast<uint32_t>(st.block_size),
                           static_cast<uint32_t>(st.blocks),
                           access,
                           st.access.nanos,
                           modified,
                           st.modification.nanos,
                           changed,
                           st.change.nanos,
                           {}};
        std::memcpy(out, &value, sizeof(value));
    }
    return WA_OK;
}

#if !defined(__EMSCRIPTEN__)
FileStat fromNative(const struct stat& info) {
#if defined(__APPLE__)
    const auto& access = info.st_atimespec;
    const auto& modified = info.st_mtimespec;
    const auto& changed = info.st_ctimespec;
    const auto device = [](dev_t dev) -> uint64_t {
        const uint32_t min = minor(dev), maj = major(dev);
        return (uint64_t{min & ~0xffu} << 12) | (maj << 8) | (min & 0xff);
    };
#else
    const auto& access = info.st_atim;
    const auto& modified = info.st_mtim;
    const auto& changed = info.st_ctim;
    const auto device = [](dev_t dev) { return static_cast<uint64_t>(dev); };
#endif
    return {device(info.st_dev),
            info.st_ino,
            info.st_mode,
            static_cast<uint32_t>(info.st_nlink),
            info.st_uid,
            info.st_gid,
            device(info.st_rdev),
            info.st_size,
            static_cast<int32_t>(info.st_blksize),
            info.st_blocks,
            {access.tv_sec, static_cast<uint32_t>(access.tv_nsec)},
            {modified.tv_sec, static_cast<uint32_t>(modified.tv_nsec)},
            {changed.tv_sec, static_cast<uint32_t>(changed.tv_nsec)}};
}
#endif

int32_t descriptorStat(int32_t fd, const std::shared_ptr<OpenFile>& file, FileStat& out) {
    if (file && file->kind != Kind::File) {
        out.mode = (file->kind == Kind::Pipe ? 0010000 : 0) | 0600;
        out.inode = static_cast<uint32_t>(file->raw);
        return WA_OK;
    }
#if defined(__EMSCRIPTEN__)
    wa_stat_t st;
    const int32_t status = wa_vfs_fstat(file ? file->raw : fd, &st);
    if (status != WA_OK) {
        return status;
    }
    if (st.size > INT64_MAX) {
        return WA_ERR_OVERFLOW;
    }
    out.mode = st.mode;
    out.size = st.size;
    out.blocks = (st.size + 511) / 512;
    out.access = out.modification = out.change = {
        st.mtime_ns / kNsPerSecond, static_cast<uint32_t>(st.mtime_ns % kNsPerSecond)};
#else
    struct stat info{};
    const int32_t status = nativeResult(::fstat(fd, &info));
    if (status != WA_OK) {
        return status;
    }
    out = fromNative(info);
#endif
    return WA_OK;
}

} // namespace

int32_t openat(int32_t dirfd, const char* path, uint32_t flags, uint32_t mode) {
    const int32_t fd = vfs_openat(dirfd, path, flags, mode);
#if defined(__EMSCRIPTEN__)
    return remember(fd, Kind::File, flags);
#else
    return fd;
#endif
}

int32_t close(int32_t fd) {
    std::shared_ptr<OpenFile> file;
    bool last = false;
    {
        std::lock_guard lock(descriptor_mutex);
        const auto it = descriptors().find(fd);
        if (it != descriptors().end()) {
            if (!it->second.file) {
                return WA_ERR_BADF;
            }
            file = std::move(it->second.file);
            // Keep the raw number closed to syscalls until the final alias
            // releases it. The underlying event descriptor still serves dups.
            if (file->kind == Kind::File || fd != file->raw) {
                descriptors().erase(it);
            }
            last = --file->descriptors == 0;
            file->close_on_release = last;
        }
    }
    if (!file) {
#if defined(__EMSCRIPTEN__)
        if (fd > 2 && !is_event_fd(fd)) {
            return WA_ERR_BADF;
        }
#endif
        return wa_vfs_close(fd);
    }
#if !defined(__EMSCRIPTEN__)
    if (fd != file->raw) {
        ::close(fd);
    }
#endif
    int32_t status = WA_OK;
    if (last && file.use_count() == 1) {
        file->close_on_release = false;
        status = closeBacking(*file);
    }
    return status;
}

int32_t pipe(int32_t* output, uint32_t flags) {
    if (!output) {
        return WA_ERR_FAULT;
    }
    int32_t ends[2];
    const int32_t status = pipe2(ends, flags | kNonblock);
    if (status != WA_OK) {
        return status;
    }
    const int32_t reader = remember(ends[0], Kind::Pipe, flags | WA_O_RDONLY);
    if (reader < 0) {
        wa_vfs_close(ends[1]);
        return reader;
    }
    const int32_t writer = remember(ends[1], Kind::Pipe, flags | WA_O_WRONLY);
    if (writer < 0) {
        close(reader);
        return writer;
    }
    output[0] = reader;
    output[1] = writer;
    return WA_OK;
}

int32_t eventfd(uint32_t value, uint32_t flags) {
    return remember(wa_eventfd(value, flags | kNonblock), Kind::Event, (flags & ~1u) | WA_O_RDWR);
}

int32_t epoll_create(uint32_t flags) {
    return remember(wa_epoll_create(flags), Kind::Epoll, flags);
}

int32_t underlying_fd(int32_t fd) {
    const auto file = lookupFile(fd);
    return file ? file->raw : fd;
}

int64_t io(int32_t fd, void* buffer, size_t count, bool write) {
    const auto file = lookupFile(fd);
    return transfer(fd, file, buffer, count, write);
}

int64_t positioned_io(int32_t fd, void* buffer, size_t count, int64_t offset, bool write) {
    const auto file = lookupFile(fd);
    const int32_t raw = file ? file->raw : fd;
    return write ? wa_vfs_pwrite(raw, buffer, count, offset)
                 : wa_vfs_pread(raw, buffer, count, offset);
}

int64_t fcntl(int32_t fd, int32_t command, int32_t argument) {
    std::lock_guard lock(descriptor_mutex);
    const auto it = descriptors().find(fd);
    if (it == descriptors().end()) {
#if defined(__EMSCRIPTEN__)
        return WA_ERR_BADF;
#else
        return nativeFcntl(fd, command, argument);
#endif
    }
    auto& descriptor = it->second;
    if (!descriptor.file) {
        return WA_ERR_BADF;
    }
    switch (command) {
    case 0:
    case 1030:
        return duplicateLocked(descriptor, argument, command == 1030);
    case 1:
        return descriptor.flags;
    case 2:
        descriptor.flags = argument & 1;
        return WA_OK;
    case 3:
        return descriptor.file->flags.load(std::memory_order_relaxed);
    case 4: {
        auto& flags = descriptor.file->flags;
        flags.store((flags.load(std::memory_order_relaxed) & ~kStatusFlags) |
                        (argument & kStatusFlags),
                    std::memory_order_relaxed);
        signalPipe();
        return WA_OK;
    }
    default:
        return WA_ERR_INVALID;
    }
}

int64_t lseek(int32_t fd, int64_t offset, int32_t whence) {
    const auto file = lookupFile(fd);
    if (file && file->kind != Kind::File) {
        return kEsPipe;
    }
    if (whence < 0 || whence > 2) {
        return WA_ERR_INVALID;
    }
#if defined(__EMSCRIPTEN__)
    if (!file) {
        return fd >= 0 && fd <= 2 ? kEsPipe : WA_ERR_BADF;
    }
    std::lock_guard lock(file->cursor_mutex);
    int64_t base = whence == 1 ? file->position : 0;
    if (whence == 2) {
        FileStat stat;
        const int32_t status = descriptorStat(fd, file, stat);
        if (status != WA_OK) {
            return status;
        }
        base = stat.size;
    }
    if (offset > INT64_MAX - base) {
        return WA_ERR_OVERFLOW;
    }
    if (offset < -base) {
        return WA_ERR_INVALID;
    }
    return file->position = base + offset;
#else
    return nativeResult(::lseek(fd, offset, whence));
#endif
}

int32_t fstat(int32_t fd, void* out, bool arm32, bool wide) {
    if (!out) {
        return WA_ERR_FAULT;
    }
    FileStat info;
    const auto file = lookupFile(fd);
    const int32_t status = descriptorStat(fd, file, info);
    return status == WA_OK ? storeStat(info, out, arm32, wide) : status;
}

int32_t fstatat(int32_t dirfd, const char* path, void* out, int32_t flags, bool arm32, bool wide) {
    if (!path || !out) {
        return WA_ERR_FAULT;
    }
    if (flags & ~(kAtSymlinkNoFollow | kAtNoAutomount | kAtEmptyPath)) {
        return WA_ERR_INVALID;
    }
    if (!path[0]) {
        if (!(flags & kAtEmptyPath)) {
            return WA_ERR_NOENT;
        }
        if (dirfd != kAtCwd) {
            return fstat(dirfd, out, arm32, wide);
        }
        path = ".";
    }
    FileStat info;
#if defined(__EMSCRIPTEN__)
    const int32_t fd = vfs_openat(dirfd, path, WA_O_RDONLY, 0);
    if (fd < 0) {
        return fd;
    }
    const int32_t status = descriptorStat(fd, nullptr, info);
    wa_vfs_close(fd);
#else
    struct stat native{};
    const int32_t status =
        nativeResult(::fstatat(dirfd == kAtCwd ? AT_FDCWD : dirfd, path, &native,
                               flags & kAtSymlinkNoFollow ? AT_SYMLINK_NOFOLLOW : 0));
    if (status == WA_OK) {
        info = fromNative(native);
    }
#endif
    return status == WA_OK ? storeStat(info, out, arm32, wide) : status;
}

int64_t vector_io(int32_t fd, const void* vectors, int32_t count, bool arm32, bool write) {
    if (count < 0 || count > kMaxIov) {
        return WA_ERR_INVALID;
    }
    if (!vectors && count) {
        return WA_ERR_FAULT;
    }
    const auto file = lookupFile(fd);
#if defined(__EMSCRIPTEN__)
    struct Slice {
        void* iov_base;
        size_t iov_len;
    };
#else
    using Slice = struct iovec;
#endif
    const size_t word_size = arm32 ? sizeof(uint32_t) : sizeof(uint64_t);
    const uint64_t ssize_max = arm32 ? INT32_MAX : INT64_MAX;
    const auto* bytes = static_cast<const uint8_t*>(vectors);
    try {
        std::vector<Slice> slices;
        slices.reserve(count);
        uint64_t total = 0;
        size_t transfer_size = 0;
        for (int32_t i = 0; i < count; ++i) {
            uint64_t address = 0, length = 0;
            std::memcpy(&address, bytes, word_size);
            std::memcpy(&length, bytes + word_size, word_size);
            bytes += 2 * word_size;
            if (length > ssize_max - total) {
                return WA_ERR_INVALID;
            }
            total += length;
            const size_t size = std::min<uint64_t>(length, kMaxTransfer - transfer_size);
            if (size && (!address || address > UINTPTR_MAX || size > UINTPTR_MAX - address)) {
                return WA_ERR_FAULT;
            }
            slices.push_back({reinterpret_cast<void*>(static_cast<uintptr_t>(address)), size});
            transfer_size += size;
        }
#if !defined(__EMSCRIPTEN__)
        if (!file && !is_event_fd(fd)) {
            return nativeResult(write ? ::writev(fd, slices.data(), count)
                                      : ::readv(fd, slices.data(), count));
        }
#endif
        if (!transfer_size) {
            return file ? 0 : transfer(fd, file, nullptr, 0, write);
        }
        // One transfer preserves pipe atomicity and stops a short read or write
        // at the exact byte where it occurred in the vector.
        std::unique_ptr<uint8_t[]> buffer(new uint8_t[transfer_size]);
        if (write) {
            size_t position = 0;
            for (const auto& [base, size] : slices) {
                if (size) {
                    std::memcpy(buffer.get() + position, base, size);
                    position += size;
                }
            }
        }
        const int64_t done = transfer(fd, file, buffer.get(), transfer_size, write);
        if (!write && done > 0) {
            size_t position = 0;
            for (const auto& [base, size] : slices) {
                const size_t copied = std::min(size, static_cast<size_t>(done) - position);
                if (copied) {
                    std::memcpy(base, buffer.get() + position, copied);
                    position += copied;
                }
            }
        }
        return done;
    } catch (const std::bad_alloc&) {
        return WA_ERR_NOMEM;
    }
}

int32_t uname(void* out, bool arm32) {
    if (!out) {
        return WA_ERR_FAULT;
    }
    struct Utsname {
        char system[65], node[65], release[65], version[65], machine[65], domain[65];
    };
    Utsname value{"Linux", "wasm-android", "6.6.0-wasm", "#1 WasmAndroid", {}, "(none)"};
    std::strcpy(value.machine, arm32 ? "armv7l" : "aarch64");
    std::memcpy(out, &value, sizeof(value));
    return WA_OK;
}

int32_t prctl(int32_t option, char* name, [[maybe_unused]] uint64_t tid) {
    constexpr int32_t kSetName = 15, kGetName = 16;
    if (option != kSetName && option != kGetName) {
        return WA_ERR_INVALID;
    }
    if (!name) {
        return WA_ERR_FAULT;
    }
    constexpr ThreadName default_name{'w', 'a', 's', 'm', '-', 'a', 'n', 'd', 'r', 'o', 'i', 'd'};
    ThreadName value{};
    if (option == kSetName) {
        for (size_t i = 0; i + 1 < value.size() && name[i]; ++i) {
            value[i] = name[i];
        }
    }
#if defined(__EMSCRIPTEN__)
    std::lock_guard lock(thread_names_mutex);
    if (!thread_names) {
        thread_names.emplace();
    }
    if (option == kSetName) {
        try {
            thread_names->insert_or_assign(tid, value);
        } catch (const std::bad_alloc&) {
            return WA_ERR_NOMEM;
        }
    } else {
        const auto it = thread_names->find(tid);
        const ThreadName& stored = it == thread_names->end() ? default_name : it->second;
        std::memcpy(name, stored.data(), stored.size());
    }
#else
    static thread_local ThreadName stored = default_name;
    if (option == kSetName) {
        stored = value;
    } else {
        std::memcpy(name, stored.data(), stored.size());
    }
#endif
    return WA_OK;
}

int32_t sleep_ns(int64_t nanoseconds, int64_t& left) {
    if (!nanoseconds) {
        return WA_OK;
    }
#if defined(__EMSCRIPTEN__)
    uint32_t sleep_word = 0;
    const int32_t status = wa_futex_wait(&sleep_word, 0, nanoseconds);
    left = 0;
    return status == WA_ERR_TIMEDOUT ? WA_OK : status;
#else
    const timespec duration{static_cast<time_t>(nanoseconds / kNsPerSecond),
                            static_cast<long>(nanoseconds % kNsPerSecond)};
    timespec remaining{};
    const int32_t status = nativeResult(::nanosleep(&duration, &remaining));
    left = remaining.tv_sec * kNsPerSecond + remaining.tv_nsec;
    return status;
#endif
}

int64_t getrandom(void* buffer, uint64_t requested, uint32_t flags) {
    constexpr uint32_t kRandom = 2, kInsecure = 4;
    if ((flags & ~7u) || (flags & (kRandom | kInsecure)) == (kRandom | kInsecure)) {
        return WA_ERR_INVALID;
    }
    const size_t count = std::min<uint64_t>(requested, kMaxTransfer);
    if (!buffer && count) {
        return WA_ERR_FAULT;
    }
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
    return nativeResult(::getrandom(buffer, count, flags));
#elif defined(__EMSCRIPTEN__)
    // The frozen worker imports have no entropy hook. Use a deterministic
    // SplitMix64 stream as the Wasm fallback, with disjoint words under contention.
    static constinit std::atomic<uint64_t> sequence{0x243f6a8885a308d3ULL};
    constexpr uint64_t step = 0x9e3779b97f4a7c15ULL;
    auto* bytes = static_cast<uint8_t*>(buffer);
    for (size_t offset = 0; offset < count; offset += sizeof(uint64_t)) {
        uint64_t word = sequence.fetch_add(step, std::memory_order_relaxed) + step;
        word = (word ^ (word >> 30)) * 0xbf58476d1ce4e5b9ULL;
        word = (word ^ (word >> 27)) * 0x94d049bb133111ebULL;
        word ^= word >> 31;
        std::memcpy(bytes + offset, &word, std::min(sizeof(word), count - offset));
    }
    return count;
#else
    if (count) {
        ::arc4random_buf(buffer, count);
    }
    return count;
#endif
}

namespace {

constexpr uintptr_t kPageMask = WA_PAGE_SIZE - 1u;
constexpr uint32_t kMapShared = 0x01, kMapPrivate = 0x02, kMapFixed = 0x10, kMapAnonymous = 0x20;
constexpr uint32_t kMapFixedNoReplace = 0x100000;
constexpr uint32_t kMapHints = 0x100 | 0x800 | 0x1000 | 0x2000 | 0x4000 | 0x8000 | 0x20000;
constexpr uint32_t kProtectionMask = 7;
constexpr size_t kBrkCapacity = 8 * 1024 * 1024;

uintptr_t pageCeil(uintptr_t value) {
    return (value + kPageMask) & ~kPageMask;
}

struct Allocation {
    explicit Allocation(size_t length)
        : data(std::aligned_alloc(WA_PAGE_SIZE, length)), size(length) {}
    ~Allocation() { std::free(data); }

    uintptr_t base() const { return reinterpret_cast<uintptr_t>(data); }
    bool covers(uintptr_t start, uintptr_t end) const {
        return base() <= start && end - base() <= size;
    }

    void* data;
    size_t size;
};

struct Region {
    uintptr_t start, end;
    uint32_t protection;
    std::shared_ptr<Allocation> allocation;
};

class AddressSpace {
  public:
    FutexMutex mutex;
    std::shared_ptr<Allocation> heap;
    uintptr_t current_brk = 0;

    bool prepare() {
        try {
            // An edit may split two edges and add one region.
            if (regions_.capacity() - regions_.size() < 3) {
                regions_.reserve(std::max(regions_.size() + 3, regions_.capacity() * 2));
            }
            return true;
        } catch (const std::bad_alloc&) {
            return false;
        }
    }

    std::shared_ptr<Allocation> backing(uintptr_t start, uintptr_t end) {
        if (heap && heap->covers(start, end)) {
            return heap;
        }
        for (const auto& area : regions_) {
            if (area.allocation->covers(start, end)) {
                return area.allocation;
            }
        }
        return nullptr;
    }

    bool overlaps(uintptr_t start, uintptr_t end) {
        const auto it = firstPast(start);
        return it != regions_.end() && it->start < end;
    }

    bool contains(uintptr_t start, uintptr_t end) {
        for (auto it = firstPast(start); it != regions_.end() && it->start <= start; ++it) {
            start = it->end;
            if (start >= end) {
                return true;
            }
        }
        return false;
    }

    void unmap(uintptr_t start, uintptr_t end) {
        split(start);
        split(end);
        regions_.erase(firstPast(start), firstPast(end));
    }

    void map(uintptr_t start, uintptr_t end, uint32_t protection,
             std::shared_ptr<Allocation> allocation) {
        unmap(start, end);
        regions_.insert(firstPast(start), Region{start, end, protection, std::move(allocation)});
        coalesce();
    }

    void protect(uintptr_t start, uintptr_t end, uint32_t protection) {
        split(start);
        split(end);
        const auto last = firstPast(end);
        for (auto it = firstPast(start); it != last; ++it) {
            it->protection = protection;
        }
        coalesce();
    }

  private:
    std::vector<Region>::iterator firstPast(uintptr_t address) {
        return std::upper_bound(regions_.begin(), regions_.end(), address,
                                [](uintptr_t addr, const Region& area) { return addr < area.end; });
    }

    void split(uintptr_t address) {
        const auto it = firstPast(address);
        if (it != regions_.end() && it->start < address) {
            Region tail{address, it->end, it->protection, it->allocation};
            it->end = address;
            regions_.insert(it + 1, std::move(tail));
        }
    }

    void coalesce() {
        size_t written = 0;
        for (size_t i = 0; i < regions_.size(); ++i) {
            if (written) {
                auto& previous = regions_[written - 1];
                const auto& current = regions_[i];
                if (previous.end == current.start && previous.protection == current.protection &&
                    previous.allocation == current.allocation) {
                    previous.end = current.end;
                    continue;
                }
            }
            if (i != written) {
                regions_[written] = std::move(regions_[i]);
            }
            ++written;
        }
        regions_.resize(written);
    }

    std::vector<Region> regions_;
};

constinit AddressSpace space;

std::shared_ptr<Allocation> allocate(size_t size) {
    try {
        auto allocation = std::make_shared<Allocation>(size);
        return allocation->data ? allocation : nullptr;
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

} // namespace

int64_t mmap(uint64_t hint, uint64_t length, uint32_t protection, uint32_t flags, uint64_t offset) {
    if (!length || protection & ~kProtectionMask || offset & kPageMask ||
        ((flags & 3) != kMapShared && (flags & 3) != kMapPrivate)) {
        return WA_ERR_INVALID;
    }
    if (!(flags & kMapAnonymous) || flags & ~(kMapShared | kMapPrivate | kMapFixed | kMapAnonymous |
                                              kMapFixedNoReplace | kMapHints)) {
        return WA_ERR_NOTSUP;
    }
    if (length > UINTPTR_MAX - kPageMask) {
        return WA_ERR_NOMEM;
    }
    const size_t size = pageCeil(length);
    const bool fixed = flags & (kMapFixed | kMapFixedNoReplace);
    if (fixed && (!hint || (hint & kPageMask) || hint > UINTPTR_MAX - size)) {
        return WA_ERR_INVALID;
    }
    std::lock_guard lock(space.mutex);
    uintptr_t start = 0;
    std::shared_ptr<Allocation> allocation;
    if (hint && hint <= UINTPTR_MAX - size) {
        start = static_cast<uintptr_t>(hint) & ~kPageMask;
        const bool overlaps = space.overlaps(start, start + size);
        if ((flags & kMapFixedNoReplace) && overlaps) {
            return WA_ERR_EXIST;
        }
        if (fixed || !overlaps) {
            allocation = space.backing(start, start + size);
        }
    }
    if (!allocation && !fixed) {
        allocation = allocate(size);
        if (allocation) {
            start = allocation->base();
        }
    }
    if (!allocation || !space.prepare()) {
        return WA_ERR_NOMEM;
    }
    // Protections are tracked in the guest map. Wasm linear memory has no
    // per-page permissions, and native guest pages can share one host page.
    std::memset(reinterpret_cast<void*>(start), 0, size);
    space.map(start, start + size, protection, std::move(allocation));
    return start;
}

int32_t munmap(uint64_t address, uint64_t length) {
    if (!length || (address & kPageMask) || length > UINTPTR_MAX - kPageMask ||
        address > UINTPTR_MAX - pageCeil(length)) {
        return WA_ERR_INVALID;
    }
    std::lock_guard lock(space.mutex);
    if (!space.prepare()) {
        return WA_ERR_NOMEM;
    }
    space.unmap(address, address + pageCeil(length));
    return WA_OK;
}

int32_t mprotect(uint64_t address, uint64_t length, uint32_t protection) {
    if ((address & kPageMask) || protection & ~kProtectionMask ||
        length > UINTPTR_MAX - kPageMask || address > UINTPTR_MAX - pageCeil(length)) {
        return WA_ERR_INVALID;
    }
    if (!length) {
        return WA_OK;
    }
    std::lock_guard lock(space.mutex);
    const uintptr_t end = address + pageCeil(length);
    if (!space.contains(address, end) || !space.prepare()) {
        return WA_ERR_NOMEM;
    }
    space.protect(address, end, protection);
    return WA_OK;
}

int64_t brk(uint64_t address) {
    std::lock_guard lock(space.mutex);
    if (!space.heap) {
        space.heap = allocate(kBrkCapacity);
        if (!space.heap) {
            return 0;
        }
        space.current_brk = space.heap->base();
    }
    if (address < space.heap->base() || address - space.heap->base() > kBrkCapacity) {
        return space.current_brk;
    }
    const uintptr_t current = pageCeil(space.current_brk);
    const uintptr_t next = pageCeil(address);
    if (current != next) {
        if ((next > current && space.overlaps(current, next)) || !space.prepare()) {
            return space.current_brk;
        }
        if (next > current) {
            std::memset(reinterpret_cast<void*>(current), 0, next - current);
            space.map(current, next, 3, space.heap);
        } else {
            space.unmap(next, current);
        }
    }
    return space.current_brk = address;
}

} // namespace wa::posix
