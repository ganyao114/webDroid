#include "kernel_internal.h"

#if !defined(__EMSCRIPTEN__)
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

#if defined(__EMSCRIPTEN__)
extern "C" {
WA_IMPORT(vfs_open) int32_t host_open(const char* path, uint32_t flags, uint32_t mode);
WA_IMPORT(vfs_read) int64_t host_read(int32_t fd, void* buf, size_t count, int64_t offset);
WA_IMPORT(vfs_write) int64_t host_write(int32_t fd, const void* buf, size_t count, int64_t offset);
WA_IMPORT(vfs_ftruncate) int32_t host_ftruncate(int32_t fd, int64_t length);
WA_IMPORT(vfs_fsync) int32_t host_fsync(int32_t fd);
WA_IMPORT(vfs_fstat) int32_t host_fstat(int32_t fd, wa_stat_t* st);
WA_IMPORT(vfs_flock) int32_t host_flock(int32_t fd, int32_t operation);
WA_IMPORT(vfs_close) int32_t host_close(int32_t fd);
}
#else
int32_t linux_error(int error) {
    switch (error) {
    case EAGAIN:
        return WA_ERR_AGAIN;
    case ENOSYS:
        return WA_ERR_NOSYS;
    case ETIMEDOUT:
        return WA_ERR_TIMEDOUT;
    case ENOTSUP:
#if EOPNOTSUPP != ENOTSUP
    case EOPNOTSUPP:
#endif
        return WA_ERR_NOTSUP;
    case EOVERFLOW:
        return WA_ERR_OVERFLOW;
    case ENAMETOOLONG:
        return -36;
    case ENOTEMPTY:
        return -39;
    case ELOOP:
        return -40;
    default:
        return -error;
    }
}

template <typename T>
T posix(T result) {
    return result < 0 ? linux_error(errno) : result;
}
#endif

bool valid_buffer(const void* buf, size_t count) {
    return (buf || !count) && count <= static_cast<size_t>(INT64_MAX);
}

}  // namespace

int32_t wa::vfs_openat(int32_t dirfd, const char* path, uint32_t flags, uint32_t mode) {
    constexpr uint32_t supported = 3 | WA_O_CREAT | WA_O_TRUNC | WA_O_APPEND |
                                   wa::kNonblock | wa::kCloexec;
    const uint32_t access = flags & 3;
    if (!path) {
        return WA_ERR_FAULT;
    }
    if ((flags & ~supported) || access == 3 || (access == WA_O_RDONLY && flags & WA_O_TRUNC)) {
        return WA_ERR_INVALID;
    }
#if defined(__EMSCRIPTEN__)
    if (dirfd != -100 && path[0] != '/') {  // AT_FDCWD
        return WA_ERR_BADF;
    }
    return host_open(path, flags, mode);
#else
    int native = access == WA_O_WRONLY ? O_WRONLY : access == WA_O_RDWR ? O_RDWR : O_RDONLY;
    if (flags & WA_O_CREAT) {
        native |= O_CREAT;
    }
    if (flags & WA_O_TRUNC) {
        native |= O_TRUNC;
    }
    if (flags & WA_O_APPEND) {
        native |= O_APPEND;
    }
    if (flags & wa::kNonblock) {
        native |= O_NONBLOCK;
    }
    if (flags & wa::kCloexec) {
        native |= O_CLOEXEC;
    }
    return posix(::openat(dirfd == -100 ? AT_FDCWD : dirfd, path, native, static_cast<mode_t>(mode)));
#endif
}

extern "C" int32_t wa_vfs_open(const char* path, uint32_t flags, uint32_t mode) {
    return wa::vfs_openat(-100, path, flags, mode);
}

extern "C" int64_t wa_vfs_read(int32_t fd, void* buf, size_t count) {
    if (!valid_buffer(buf, count)) {
        return WA_ERR_FAULT;
    }
    if (wa::is_event_fd(fd)) {
        return wa::event_read(fd, buf, count);
    }
#if defined(__EMSCRIPTEN__)
    return host_read(fd, buf, count, -1);
#else
    return posix(::read(fd, buf, count));
#endif
}

extern "C" int64_t wa_vfs_write(int32_t fd, const void* buf, size_t count) {
    if (!valid_buffer(buf, count)) {
        return WA_ERR_FAULT;
    }
    if (wa::is_event_fd(fd)) {
        return wa::event_write(fd, buf, count);
    }
#if defined(__EMSCRIPTEN__)
    return host_write(fd, buf, count, -1);
#else
    return posix(::write(fd, buf, count));
#endif
}

extern "C" int64_t wa_vfs_pread(int32_t fd, void* buf, size_t count, int64_t offset) {
    if (offset < 0 || !valid_buffer(buf, count)) {
        return offset < 0 ? WA_ERR_INVALID : WA_ERR_FAULT;
    }
    if (wa::is_event_fd(fd)) {
        return -29;  // ESPIPE
    }
#if defined(__EMSCRIPTEN__)
    return host_read(fd, buf, count, offset);
#else
    return posix(::pread(fd, buf, count, offset));
#endif
}

extern "C" int64_t wa_vfs_pwrite(int32_t fd, const void* buf, size_t count, int64_t offset) {
    if (offset < 0 || !valid_buffer(buf, count)) {
        return offset < 0 ? WA_ERR_INVALID : WA_ERR_FAULT;
    }
    if (wa::is_event_fd(fd)) {
        return -29;  // ESPIPE
    }
#if defined(__EMSCRIPTEN__)
    return host_write(fd, buf, count, offset);
#else
    return posix(::pwrite(fd, buf, count, offset));
#endif
}

extern "C" int32_t wa_vfs_ftruncate(int32_t fd, int64_t length) {
    if (length < 0 || wa::is_event_fd(fd)) {
        return WA_ERR_INVALID;
    }
#if defined(__EMSCRIPTEN__)
    return host_ftruncate(fd, length);
#else
    return posix(::ftruncate(fd, length));
#endif
}

extern "C" int32_t wa_vfs_fsync(int32_t fd) {
    if (wa::is_event_fd(fd)) {
        return WA_ERR_INVALID;
    }
#if defined(__EMSCRIPTEN__)
    return host_fsync(fd);
#else
    return posix(::fsync(fd));
#endif
}

extern "C" int32_t wa_vfs_fstat(int32_t fd, wa_stat_t* st) {
    if (!st) {
        return WA_ERR_FAULT;
    }
#if defined(__EMSCRIPTEN__)
    return host_fstat(fd, st);
#else
    struct stat info {};
    if (const int32_t result = posix(::fstat(fd, &info)); result < 0) {
        return result;
    }
#if defined(__APPLE__)
    const auto& mtime = info.st_mtimespec;
#else
    const auto& mtime = info.st_mtim;
#endif
    *st = {static_cast<uint64_t>(info.st_size), static_cast<uint32_t>(info.st_mode), 0,
           mtime.tv_sec * wa::kNsPerSecond + mtime.tv_nsec};
    return WA_OK;
#endif
}

extern "C" int32_t wa_vfs_flock(int32_t fd, int32_t operation) {
    if (wa::is_event_fd(fd)) {
        return WA_ERR_INVALID;
    }
#if defined(__EMSCRIPTEN__)
    return host_flock(fd, operation);
#else
    return posix(::flock(fd, operation));
#endif
}

extern "C" int32_t wa_vfs_close(int32_t fd) {
    if (wa::is_event_fd(fd)) {
        return wa::event_close(fd);
    }
#if defined(__EMSCRIPTEN__)
    return host_close(fd);
#else
    return posix(::close(fd));
#endif
}
