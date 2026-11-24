#include "bionic_internal.h"
#include "elf_image.h"
#include "softmmu_internal.h"

#include <cerrno>
#include <climits>
#include <exception>
#include <fcntl.h>
#include <ftw.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/uio.h>
#include <termios.h>
#include <unistd.h>
#include <utility>

#if defined(__APPLE__)
#include <sys/mount.h>
#else
#include <sys/statfs.h>
#endif

namespace wa::dbt {

int OpenMountedFile(std::span<const uint8_t> bytes, int flags) {
    if ((flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC))) {
        errno = EROFS;
        return -1;
    }
    if (flags & O_DIRECTORY) {
        errno = ENOTDIR;
        return -1;
    }
    // Give Bionic's descriptor operations a private, seekable snapshot of data
    // from the VM filesystem. File-backed APK entries use the ELF reader.
    char path[] = "/tmp/wa-native-file-XXXXXX";
    const int backing = mkstemp(path);
    if (backing < 0) return -1;
    while (!bytes.empty()) {
        const auto count = write(backing, bytes.data(), bytes.size());
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        bytes = bytes.subspan(count);
    }
    if (!bytes.empty()) errno = EIO;
    const int fd = bytes.empty() ? open(path, flags) : -1;
    const int error = errno;
    close(backing);
    unlink(path);
    errno = error;
    return fd;
}

namespace {

constexpr int kAtCwd = -100;
constexpr size_t kIoLimit = 16 << 20;
constexpr size_t kIovLimit = 1024;

int AtFd(const wa_host_call_ctx_t& ctx, uint64_t fd) {
    return static_cast<int32_t>(fd) == kAtCwd ? AT_FDCWD : HostFd(*ctx.dbt, fd);
}

std::string Path(const wa_host_call_ctx_t& ctx, uint64_t address, int directory = AT_FDCWD) {
    auto path = GuestMemory(ctx).String(address);
    if (!path.empty() && path.front() != '/' && directory == AT_FDCWD &&
        !ctx.dbt->bionic->cwd.empty()) {
        path = ctx.dbt->bionic->cwd + "/" + path;
    }
    return path;
}

std::optional<std::span<const uint8_t>> MountedFile(const wa_host_call_ctx_t& ctx,
                                                    std::string_view path) {
    const auto& read = ctx.dbt->bionic->read_guest_file;
    return read && path.starts_with('/') ? read(path) : std::nullopt;
}

bool StatMountedFile(const wa_host_call_ctx_t& ctx, std::string_view path, struct stat& result) {
    const auto data = MountedFile(ctx, path);
    if (!data) return false;
    result.st_mode = S_IFREG | 0444;
    result.st_nlink = 1;
    result.st_size = data->size();
    result.st_blksize = WA_PAGE_SIZE;
    result.st_blocks = (data->size() + 511) / 512;
    return true;
}

struct Flag {
    int guest, host;
};
constexpr Flag kOpenFlags[] = {
    {00000100, O_CREAT},    {00000200, O_EXCL},     {00000400, O_NOCTTY}, {00001000, O_TRUNC},
    {00002000, O_APPEND},   {00004000, O_NONBLOCK}, {00010000, O_DSYNC},  {00040000, O_DIRECTORY},
    {00100000, O_NOFOLLOW}, {02000000, O_CLOEXEC},  {04000000, O_SYNC},
};

int OpenFlags(int guest) {
    int result = guest & O_ACCMODE;
    for (const auto flag : kOpenFlags)
        if (guest & flag.guest)
            result |= flag.host;
    return result;
}

int GuestOpenFlags(int host) {
    int result = host & O_ACCMODE;
    for (const auto flag : kOpenFlags)
        if (flag.host && (host & flag.host) == flag.host)
            result |= flag.guest;
    return result;
}

enum class Open { Plain, Checked, At, AtChecked };
template <Open op> void OpenFile(wa_host_call_ctx_t& ctx) {
    constexpr bool at = op == Open::At || op == Open::AtChecked;
    constexpr bool checked = op == Open::Checked || op == Open::AtChecked;
    const auto fd = at ? AtFd(ctx, ctx.args[0]) : AT_FDCWD;
    const auto flags = static_cast<int>(ctx.args[at ? 2 : 1]);
    if (checked && (flags & (00000100 | 020000000)))
        throw GuestFault{WA_ERR_FAULT};
    if (flags & 020000000)
        return Fail(ctx, ENOTSUP);
    const auto path = Path(ctx, ctx.args[at ? 1 : 0], fd);
    const auto mode = checked ? 0 : static_cast<mode_t>(ctx.args[at ? 3 : 2]);
    const auto host_flags = OpenFlags(flags);
    const auto mounted = MountedFile(ctx, path);
    const auto result = mounted ? OpenMountedFile(*mounted, host_flags) :
        openat(fd, path.c_str(), host_flags, mode);
    PosixResult(ctx, AdoptFd(*ctx.dbt, result));
}

bool TransferEvent(wa_host_call_ctx_t& ctx, int descriptor, uint64_t address, uint64_t count,
                   bool read) {
    auto& dbt = *ctx.dbt;
    auto event = EventFor(dbt, descriptor);
    if (!event)
        return false;
    const GuestMemory mem(ctx);
    if (count < sizeof(uint64_t)) {
        Fail(ctx, EINVAL);
        return true;
    }
    const auto wait = [&](WaitKind kind) {
        if (event->nonblocking) {
            Fail(ctx, EAGAIN);
            return false;
        }
        if (const auto error = Park(dbt, kind, event->id)) {
            SetErrno(dbt, error);
            ctx.ret0 = UINT64_MAX;
            return false;
        }
        return true;
    };
    if (read) {
        while (!event->count)
            if (!wait(WaitKind::EventRead))
                return true;
        const auto value = event->semaphore ? 1 : event->count;
        mem.Write<uint64_t>(address, value);
        event->count -= value;
        if (!event->count) {
            char token;
            ::read(HostFd(dbt, descriptor), &token, 1);
        }
        Wake(dbt, WaitKind::EventWrite, event->id);
    } else {
        const auto value = mem.Read<uint64_t>(address);
        if (value == UINT64_MAX) {
            Fail(ctx, EINVAL);
            return true;
        }
        while (value >= UINT64_MAX - event->count)
            if (!wait(WaitKind::EventWrite))
                return true;
        if (value && !event->count) {
            const char token = 1;
            if (::write(event->notification, &token, 1) != 1) {
                PosixResult(ctx, -1);
                return true;
            }
        }
        event->count += value;
        if (event->count)
            Wake(dbt, WaitKind::EventRead, event->id);
    }
    dbt.running->wait = {};
    ctx.ret0 = sizeof(uint64_t);
    return true;
}

enum class Io { Read, ReadChecked, Write, WriteChecked, Pread, Pwrite, Pread64, Pwrite64 };
template <Io op> void Transfer(wa_host_call_ctx_t& ctx) {
    constexpr bool read =
        op == Io::Read || op == Io::ReadChecked || op == Io::Pread || op == Io::Pread64;
    constexpr bool positioned =
        op == Io::Pread || op == Io::Pread64 || op == Io::Pwrite || op == Io::Pwrite64;
    const GuestMemory mem(ctx);
    CallReader args(ctx);
    const auto descriptor = static_cast<int>(args.Next());
    const auto fd = HostFd(*ctx.dbt, descriptor);
    if (fd < 0)
        return PosixResult(ctx, -1);
    const auto address = args.Next(), count = args.Next();
    if constexpr (op == Io::ReadChecked || op == Io::WriteChecked)
        if (count > args.Next())
            throw GuestFault{WA_ERR_FAULT};
    if constexpr (!positioned)
        if (TransferEvent(ctx, descriptor, address, count, read))
            return;
    const auto size = std::min<uint64_t>(count, kIoLimit);
    std::vector<uint8_t> bytes(size);
    if constexpr (!read)
        mem.Read(address, bytes.data(), size);
    ssize_t result;
    if constexpr (positioned) {
        const auto offset = op == Io::Pread64 || op == Io::Pwrite64
                                ? static_cast<int64_t>(args.Next(ArgKind::Long))
                                : SignedWord(mem, args.Next());
        if constexpr (read)
            result = pread(fd, bytes.data(), size, offset);
        else
            result = pwrite(fd, bytes.data(), size, offset);
    } else if constexpr (read) {
        result = ::read(fd, bytes.data(), size);
    } else {
        result = ::write(fd, bytes.data(), size);
    }
    PosixResult(ctx, result);
    if constexpr (read)
        if (result > 0)
            mem.Write(address, bytes.data(), result);
}

template <bool Read> void VectorTransfer(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    const auto count = static_cast<int32_t>(ctx.args[2]);
    if (count < 0 || count > int(kIovLimit))
        return Fail(ctx, EINVAL);
    std::vector<iovec> vectors(count);
    std::vector<uint64_t> addresses(count);
    std::vector<std::vector<uint8_t>> buffers(count);
    size_t available = kIoLimit;
    for (int i = 0; i < count; ++i) {
        const auto address = ctx.args[1] + i * 2 * mem.width();
        addresses[i] = mem.ReadWord(address);
        const auto size =
            static_cast<size_t>(std::min<uint64_t>(mem.ReadWord(address + mem.width()), available));
        available -= size;
        buffers[i].resize(size);
        vectors[i] = {buffers[i].data(), size};
        if constexpr (!Read)
            mem.Read(addresses[i], vectors[i].iov_base, size);
    }
    const auto fd = HostFd(*ctx.dbt, ctx.args[0]);
    const auto result = Read ? readv(fd, vectors.data(), count) : writev(fd, vectors.data(), count);
    PosixResult(ctx, result);
    if constexpr (Read) {
        auto remaining = std::max<ssize_t>(0, result);
        for (int i = 0; i < count && remaining; ++i) {
            const auto size = std::min<size_t>(remaining, vectors[i].iov_len);
            mem.Write(addresses[i], vectors[i].iov_base, size);
            remaining -= size;
        }
    }
}

template <auto Function> void FdCall(wa_host_call_ctx_t& ctx) {
    if constexpr (std::is_invocable_v<decltype(Function), int, int>)
        PosixResult(ctx, Function(HostFd(*ctx.dbt, ctx.args[0]), static_cast<int>(ctx.args[1])));
    else
        PosixResult(ctx, Function(HostFd(*ctx.dbt, ctx.args[0])));
}

template <bool Wide, bool Truncate> void OffsetCall(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    CallReader args(ctx);
    const auto fd = HostFd(*ctx.dbt, args.Next());
    const auto offset =
        Wide ? static_cast<int64_t>(args.Next(ArgKind::Long)) : SignedWord(mem, args.Next());
    if constexpr (Truncate) {
        PosixResult(ctx, ftruncate(fd, offset));
    } else {
        const auto result = lseek(fd, offset, args.Next());
        if (!Wide && mem.width() == 4 && result > INT32_MAX)
            return Fail(ctx, EOVERFLOW);
        PosixResult(ctx, result);
        if constexpr (Wide)
            SetScalarResult(ctx, ArgKind::Long, ctx.ret0);
    }
}

void WriteStat(GuestMemory mem, uint64_t address, const struct stat& st) {
#if defined(__APPLE__)
    const timespec times[] = {st.st_atimespec, st.st_mtimespec, st.st_ctimespec};
#else
    const timespec times[] = {st.st_atim, st.st_mtim, st.st_ctim};
#endif
    const bool wide = mem.width() == 8;
    const std::array<uint8_t, 128> zeros{};
    mem.Write(address, zeros.data(), wide ? 128 : 104);
    mem.Write<uint64_t>(address, st.st_dev);
    if (wide)
        mem.Write<uint64_t>(address + 8, st.st_ino);
    else {
        mem.Write<uint32_t>(address + 12, st.st_ino);
        mem.Write<uint64_t>(address + 96, st.st_ino);
    }
    mem.Write<uint32_t>(address + 16, st.st_mode);
    mem.Write<uint32_t>(address + 20, st.st_nlink);
    mem.Write<uint32_t>(address + 24, st.st_uid);
    mem.Write<uint32_t>(address + 28, st.st_gid);
    mem.Write<uint64_t>(address + 32, st.st_rdev);
    mem.Write<int64_t>(address + 48, st.st_size);
    mem.Write<int32_t>(address + 56, st.st_blksize);
    mem.Write<int64_t>(address + 64, st.st_blocks);
    for (size_t i = 0; i < 3; ++i) {
        const auto slot = address + 72 + 2 * mem.width() * i;
        mem.WriteWord(slot, times[i].tv_sec);
        mem.WriteWord(slot + mem.width(), times[i].tv_nsec);
    }
}

struct FileTreeWalk {
    wa_host_call_ctx_t& ctx;
    size_t prefix;
    std::exception_ptr error;
};

thread_local FileTreeWalk* current_walk = nullptr;

int VisitFile(const char* path, const struct stat* stat, int type, struct FTW* position) {
    auto& walk = *current_walk;
    try {
        int guest_type;
        switch (type) {
        case FTW_F: guest_type = 0; break;
        case FTW_D: guest_type = 1; break;
        case FTW_DNR: guest_type = 2; break;
        case FTW_DP: guest_type = 3; break;
        case FTW_NS: guest_type = 4; break;
        case FTW_SL: guest_type = 5; break;
        case FTW_SLN: guest_type = 6; break;
        default: return -1;
        }
        auto& dbt = *walk.ctx.dbt;
        const GuestMemory mem(walk.ctx);
        const std::string_view full_path(path);
        const auto prefix = std::min(walk.prefix, full_path.size());
        const auto name = full_path.substr(prefix);
        GuestBuffer guest_path(dbt, name.size() + 1), guest_stat(dbt, 128), guest_position(dbt, 8);
        mem.Write(guest_path.address, name.data(), name.size() + 1);
        const struct stat empty{};
        WriteStat(mem, guest_stat.address, stat && type != FTW_NS ? *stat : empty);
        mem.Write<int32_t>(guest_position.address, std::max<int64_t>(0, position->base - int64_t(prefix)));
        mem.Write<int32_t>(guest_position.address + 4, position->level);
        const uint64_t args[] = {guest_path.address, guest_stat.address,
                                 static_cast<uint64_t>(guest_type), guest_position.address};
        uint64_t result;
        CheckGuest(wa_dbt_call(&dbt, walk.ctx.args[1], args, std::size(args), &result));
        return static_cast<int32_t>(result);
    } catch (...) {
        walk.error = std::current_exception();
        return -1;
    }
}

void WalkFiles(wa_host_call_ctx_t& ctx) {
    const auto flags = ctx.args[3];
    if (flags & ~0xfu || static_cast<int32_t>(ctx.args[2]) <= 0)
        return Fail(ctx, EINVAL);
    if (flags & 8) // FTW_CHDIR changes the process-wide working directory on the host.
        return Fail(ctx, ENOTSUP);
    const auto path = Path(ctx, ctx.args[0]);
    const auto prefix = path.size() - GuestMemory(ctx).String(ctx.args[0]).size();
    FileTreeWalk walk{ctx, prefix, {}};
    const auto previous = std::exchange(current_walk, &walk);
    const auto result = nftw(path.c_str(), VisitFile, static_cast<int32_t>(ctx.args[2]),
                            (flags & 1 ? FTW_PHYS : 0) | (flags & 2 ? FTW_MOUNT : 0) |
                            (flags & 4 ? FTW_DEPTH : 0));
    current_walk = previous;
    if (walk.error)
        std::rethrow_exception(walk.error);
    PosixResult(ctx, result);
}

enum class Stat { File, Link, Descriptor, At };
template <Stat op> void StatCall(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    struct stat result{};
    int status;
    if constexpr (op == Stat::Descriptor) {
        status = fstat(HostFd(*ctx.dbt, ctx.args[0]), &result);
    } else if constexpr (op == Stat::At) {
        const auto fd = AtFd(ctx, ctx.args[0]);
        const auto path = Path(ctx, ctx.args[1], fd);
        const auto flags = ctx.args[3];
        if ((flags & 0x1000) && path.empty())
            status = fstat(fd, &result);
        else if (StatMountedFile(ctx, path, result))
            status = 0;
        else
            status = fstatat(fd, path.c_str(), &result, flags & 0x100 ? AT_SYMLINK_NOFOLLOW : 0);
    } else {
        const auto path = Path(ctx, ctx.args[0]);
        status = StatMountedFile(ctx, path, result) ? 0 :
            op == Stat::Link ? lstat(path.c_str(), &result) : stat(path.c_str(), &result);
    }
    PosixResult(ctx, status);
    if (!status)
        WriteStat(mem, ctx.args[op == Stat::At ? 2 : 1], result);
}

void Statvfs(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    const auto path = Path(ctx, ctx.args[0]);
    struct statvfs info{};
    if (statvfs(path.c_str(), &info))
        return PosixResult(ctx, -1);
    const uint64_t values[] = {info.f_bsize,  info.f_frsize, info.f_blocks, info.f_bfree,
                               info.f_bavail, info.f_files,  info.f_ffree,  info.f_favail,
                               info.f_fsid,   info.f_flag,   info.f_namemax};
    const auto address = ctx.args[1];
    for (size_t i = 0; i < std::size(values); ++i)
        mem.WriteWord(address + i * mem.width(), values[i]);
    if (mem.width() == 8) {
        const std::array<uint32_t, 6> reserved{};
        mem.Write(address + std::size(values) * mem.width(), reserved.data(), sizeof(reserved));
    }
}

template<bool ByPath> void Statfs(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    struct statfs info{};
    const auto status = ByPath ? statfs(Path(ctx, ctx.args[0]).c_str(), &info)
                               : fstatfs(HostFd(*ctx.dbt, ctx.args[0]), &info);
    if (status)
        return PosixResult(ctx, -1);
    const auto width = mem.width();
    const auto address = ctx.args[1];
    std::array<uint8_t, 120> zeros{};
    mem.Write(address, zeros.data(), width == 8 ? 120 : 88);
    mem.WriteWord(address, info.f_type);
    mem.WriteWord(address + width, info.f_bsize);
    const uint64_t sizes[] = {info.f_blocks, info.f_bfree, info.f_bavail, info.f_files,
                              info.f_ffree};
    mem.Write(address + 2 * width, sizes, sizeof(sizes));
    const auto trailer = address + 2 * width + sizeof(sizes);
    mem.Write(trailer, &info.f_fsid, 8);
#if defined(__APPLE__)
    mem.WriteWord(trailer + 8, 255);
    mem.WriteWord(trailer + 8 + width, info.f_bsize);
    mem.WriteWord(trailer + 8 + 2 * width, info.f_flags & MNT_RDONLY ? 1 : 0);
#else
    mem.WriteWord(trailer + 8, info.f_namelen);
    mem.WriteWord(trailer + 8 + width, info.f_frsize);
    mem.WriteWord(trailer + 8 + 2 * width, info.f_flags);
#endif
}

enum class FilePath {
    Access,
    Unlink,
    Mkdir,
    Rmdir,
    Remove,
    Rename,
    Link,
    Symlink,
    Chmod,
    UnlinkAt,
    ChmodAt,
    Truncate,
    Truncate64,
    Readlink,
    Realpath,
    Chdir,
    Pathconf,
    Utime,
    Utimes,
    UtimensAt
};
template <FilePath op> void PathCall(wa_host_call_ctx_t& ctx) {
    constexpr bool at =
        op == FilePath::UnlinkAt || op == FilePath::ChmodAt || op == FilePath::UtimensAt;
    const GuestMemory mem(ctx);
    const auto fd = at ? AtFd(ctx, ctx.args[0]) : AT_FDCWD;
    const auto path = Path(ctx, ctx.args[at ? 1 : 0], fd);
    const auto p = path.c_str();
    int64_t result = 0;
    if constexpr (op == FilePath::Access) {
        if (MountedFile(ctx, path)) {
            if (ctx.args[1] & (W_OK | X_OK)) return Fail(ctx, EACCES);
            ctx.ret0 = 0;
            return;
        }
        result = access(p, ctx.args[1]);
    }
    if constexpr (op == FilePath::Unlink)
        result = unlink(p);
    if constexpr (op == FilePath::Mkdir)
        result = mkdir(p, ctx.args[1]);
    if constexpr (op == FilePath::Rmdir)
        result = rmdir(p);
    if constexpr (op == FilePath::Remove)
        result = std::remove(p);
    if constexpr (op == FilePath::Rename)
        result = std::rename(p, Path(ctx, ctx.args[1]).c_str());
    if constexpr (op == FilePath::Link)
        result = link(p, Path(ctx, ctx.args[1]).c_str());
    if constexpr (op == FilePath::Symlink)
        result = symlink(mem.String(ctx.args[0]).c_str(), Path(ctx, ctx.args[1]).c_str());
    if constexpr (op == FilePath::Chmod)
        result = chmod(p, ctx.args[1]);
    if constexpr (op == FilePath::UnlinkAt)
        result = unlinkat(fd, p, ctx.args[2] & 0x200 ? AT_REMOVEDIR : 0);
    if constexpr (op == FilePath::ChmodAt)
        result = fchmodat(fd, p, ctx.args[2], ctx.args[3] & 0x100 ? AT_SYMLINK_NOFOLLOW : 0);
    if constexpr (op == FilePath::Truncate)
        result = truncate(p, SignedWord(mem, ctx.args[1]));
    if constexpr (op == FilePath::Truncate64) {
        CallReader args(ctx);
        args.Next();
        result = truncate(p, static_cast<int64_t>(args.Next(ArgKind::Long)));
    }
    if constexpr (op == FilePath::Pathconf) {
        const auto command = ctx.args[1];
        if (command < 1 || command > 9)
            return Fail(ctx, EINVAL);
        const int options[] = {_PC_LINK_MAX,         _PC_MAX_CANON, _PC_MAX_INPUT,
                               _PC_NAME_MAX,         _PC_PATH_MAX,  _PC_PIPE_BUF,
                               _PC_CHOWN_RESTRICTED, _PC_NO_TRUNC,  _PC_VDISABLE};
        errno = 0;
        result = pathconf(p, options[command - 1]);
    }
    if constexpr (op == FilePath::Readlink) {
        std::vector<char> bytes(std::min<uint64_t>(ctx.args[2], kIoLimit));
        result = readlink(p, bytes.data(), bytes.size());
        if (result > 0)
            mem.Write(ctx.args[1], bytes.data(), result);
    }
    if constexpr (op == FilePath::Realpath || op == FilePath::Chdir) {
        std::unique_ptr<char, decltype(&std::free)> resolved(realpath(p, nullptr), std::free);
        if (!resolved) {
            SetErrno(*ctx.dbt, GuestErrno(errno));
            if constexpr (op == FilePath::Chdir)
                ctx.ret0 = UINT64_MAX;
            return;
        }
        if constexpr (op == FilePath::Chdir) {
            struct stat st{};
            if (stat(resolved.get(), &st))
                return PosixResult(ctx, -1);
            if (!S_ISDIR(st.st_mode))
                return Fail(ctx, ENOTDIR);
            ctx.dbt->bionic->cwd = resolved.get();
        } else if (ctx.args[1]) {
            mem.Write(ctx.args[1], resolved.get(), std::strlen(resolved.get()) + 1);
            ctx.ret0 = ctx.args[1];
        } else {
            ctx.ret0 = CopyString(*ctx.dbt, resolved.get());
        }
        return;
    }
    if constexpr (op == FilePath::Utime || op == FilePath::Utimes || op == FilePath::UtimensAt) {
        constexpr bool seconds_only = op == FilePath::Utime;
        const auto source = ctx.args[at ? 2 : 1];
        struct timespec times[2]{};
        if (source) {
            for (size_t i = 0; i < 2; ++i) {
                const auto address = source + i * mem.width() * (seconds_only ? 1 : 2);
                const auto fraction = seconds_only ? 0 : SignedWord(mem, mem.ReadWord(address + mem.width()));
                if (!at && (fraction < 0 || fraction >= 1000000))
                    return Fail(ctx, EINVAL);
                times[i].tv_sec = SignedWord(mem, mem.ReadWord(address));
                times[i].tv_nsec = at ? fraction : fraction * 1000;
                if (at && fraction == 0x3fffffff)
                    times[i].tv_nsec = UTIME_NOW;
                if (at && fraction == 0x3ffffffe)
                    times[i].tv_nsec = UTIME_OMIT;
            }
        }
        result = utimensat(fd, p, source ? times : nullptr,
                           at && (ctx.args[3] & 0x100) ? AT_SYMLINK_NOFOLLOW : 0);
    }
    if constexpr (op == FilePath::Pathconf) {
        if (result < 0 && !errno) {
            ctx.ret0 = UINT64_MAX;
            return;
        }
    }
    PosixResult(ctx, result);
}

void Getcwd(wa_host_call_ctx_t& ctx) {
    auto path = ctx.dbt->bionic->cwd;
    if (path.empty()) {
        std::unique_ptr<char, decltype(&std::free)> host(getcwd(nullptr, 0), std::free);
        if (!host) {
            SetErrno(*ctx.dbt, GuestErrno(errno));
            return;
        }
        path = host.get();
    }
    const auto destination = ctx.args[0], capacity = ctx.args[1];
    if ((!capacity && destination) || (capacity && capacity <= path.size())) {
        SetErrno(*ctx.dbt, GuestErrno(ERANGE));
        return;
    }
    ctx.ret0 =
        destination ? destination : AllocateGuest(*ctx.dbt, capacity ? capacity : path.size() + 1);
    if (ctx.ret0)
        GuestMemory(ctx).Write(ctx.ret0, path.c_str(), path.size() + 1);
    else
        SetErrno(*ctx.dbt, GuestErrno(ENOMEM));
}

void Mkstemp(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    auto path = Path(ctx, ctx.args[0]);
    const auto length = mem.String(ctx.args[0]).size();
    const auto fd = mkstemp(path.data());
    const auto guest = AdoptFd(*ctx.dbt, fd);
    PosixResult(ctx, guest);
    if (guest >= 0)
        mem.Write(ctx.args[0], path.c_str() + path.size() - length, length + 1);
}

void Mkdtemp(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    auto path = Path(ctx, ctx.args[0]);
    const auto length = mem.String(ctx.args[0]).size();
    if (!mkdtemp(path.data())) {
        SetErrno(*ctx.dbt, GuestErrno(errno));
        return;
    }
    mem.Write(ctx.args[0], path.c_str() + path.size() - length, length + 1);
    ctx.ret0 = ctx.args[0];
}

void Tempnam(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    const auto dir = ctx.args[0] ? Path(ctx, ctx.args[0]) : "/tmp";
    const auto prefix = ctx.args[1] ? mem.String(ctx.args[1], 255, true) : "";
    auto path = dir + "/" + prefix + "XXXXXX";
    const auto fd = mkstemp(path.data());
    if (fd < 0) {
        SetErrno(*ctx.dbt, GuestErrno(errno));
        return;
    }
    close(fd);
    unlink(path.c_str());
    ctx.ret0 = CopyString(*ctx.dbt, path);
}

enum class Directory { Open, FdOpen, Read, Reentrant, Close, Rewind, Descriptor };
template <Directory op> void DirectoryCall(wa_host_call_ctx_t& ctx) {
    auto& dbt = *ctx.dbt;
    const GuestMemory mem(ctx);
    auto& dirs = dbt.bionic->directories;
    if constexpr (op == Directory::Open || op == Directory::FdOpen) {
        DIR* dir;
        if constexpr (op == Directory::Open)
            dir = opendir(Path(ctx, ctx.args[0]).c_str());
        else
            dir = fdopendir(HostFd(dbt, ctx.args[0]));
        if (!dir) {
            SetErrno(dbt, GuestErrno(errno));
            return;
        }
        int descriptor = -1;
        try {
            if constexpr (op == Directory::Open) {
                descriptor = AdoptFd(dbt, dirfd(dir), false);
                if (descriptor < 0) {
                    const auto error = errno;
                    closedir(dir);
                    SetErrno(dbt, GuestErrno(error));
                    return;
                }
            } else {
                descriptor = ctx.args[0];
                dbt.bionic->descriptors.at(descriptor).owned = false;
            }
            GuestBuffer storage(dbt, 280);
            dirs.emplace(storage.address, GuestDirectory{dir, descriptor, storage.address});
            ctx.ret0 = std::exchange(storage.address, 0);
        } catch (...) {
            closedir(dir);
            dbt.bionic->descriptors.erase(descriptor);
            throw;
        }
    } else {
        auto it = dirs.find(ctx.args[0]);
        if (it == dirs.end()) {
            if constexpr (op == Directory::Read)
                SetErrno(dbt, GuestErrno(EBADF));
            else if constexpr (op == Directory::Reentrant)
                ctx.ret0 = GuestErrno(EBADF);
            else
                Fail(ctx, EBADF);
            return;
        }
        auto& [dir, descriptor, entry] = it->second;
        if constexpr (op == Directory::Close) {
            PosixResult(ctx, closedir(dir));
            dbt.bionic->descriptors.erase(descriptor);
            FreeGuest(dbt, entry);
            dirs.erase(it);
        } else if constexpr (op == Directory::Rewind) {
            rewinddir(dir);
        } else if constexpr (op == Directory::Descriptor) {
            ctx.ret0 = descriptor;
        } else {
            errno = 0;
            const auto* item = readdir(dir);
            const auto output = op == Directory::Reentrant ? ctx.args[1] : entry;
            if (item) {
                const auto name_length = std::strlen(item->d_name);
                if (name_length > 255)
                    throw GuestFault{WA_ERR_OVERFLOW};
                mem.Write<uint64_t>(output, item->d_ino);
#if defined(__APPLE__)
                mem.Write<int64_t>(output + 8, item->d_seekoff);
#else
                mem.Write<int64_t>(output + 8, item->d_off);
#endif
                mem.Write<uint16_t>(output + 16, AlignUp(20 + name_length, 8));
                mem.Write<uint8_t>(output + 18, item->d_type);
                mem.Write(output + 19, item->d_name, name_length + 1);
            } else if (errno && op == Directory::Read) {
                SetErrno(dbt, GuestErrno(errno));
            }
            if constexpr (op == Directory::Reentrant) {
                const auto error = GuestErrno(errno);
                mem.WriteWord(ctx.args[2], item ? output : 0);
                if (!item)
                    ctx.ret0 = error;
            } else {
                ctx.ret0 = item ? output : 0;
            }
        }
    }
}

void Close(wa_host_call_ctx_t& ctx) {
    PosixResult(ctx, CloseFd(*ctx.dbt, static_cast<int32_t>(ctx.args[0])));
}

template <bool Exact> void DuplicateFd(wa_host_call_ctx_t& ctx) {
    auto& dbt = *ctx.dbt;
    const auto source = static_cast<int32_t>(ctx.args[0]);
    const auto fd = HostFd(dbt, source);
    if (fd < 0)
        return PosixResult(ctx, -1);
    if constexpr (Exact) {
        const auto target = static_cast<int32_t>(ctx.args[1]);
        if (target < 0)
            return Fail(ctx, EBADF);
        if (target != source) {
            auto& descriptors = dbt.bionic->descriptors;
            auto entry = descriptors.find(target);
            // Never overwrite an original host standard descriptor.
            if (entry != descriptors.end() && entry->second.host >= 3 && !entry->second.event) {
                if (dup2(fd, entry->second.host) < 0)
                    return PosixResult(ctx, -1);
                entry->second.event = EventFor(dbt, source);
            } else {
                const auto copy = dup(fd);
                if (copy < 0)
                    return PosixResult(ctx, -1);
                const auto event = EventFor(dbt, source);
                try {
                    if (entry != descriptors.end() && entry->second.owned)
                        close(entry->second.host);
                    descriptors.insert_or_assign(target, GuestDescriptor{copy, true, event});
                } catch (...) {
                    close(copy);
                    throw;
                }
            }
        }
        ctx.ret0 = target;
    } else {
        const auto copy = AdoptFd(dbt, dup(fd));
        if (copy >= 0)
            dbt.bionic->descriptors.at(copy).event = EventFor(dbt, source);
        PosixResult(ctx, copy);
    }
}

void FcntlLock(wa_host_call_ctx_t& ctx, int fd, int operation, bool wide) {
    const GuestMemory mem(ctx);
    const auto pointer = ctx.args[2];
    wide = wide || mem.width() == 8;
    const auto start = wide ? 8 : 4;
    const auto length = wide ? 8 : 4;
    const auto read = [&](uint64_t offset) {
        return wide ? mem.Read<int64_t>(pointer + offset) : mem.Read<int32_t>(pointer + offset);
    };
    const auto type = mem.Read<int16_t>(pointer);
    const auto whence = mem.Read<int16_t>(pointer + 2);
    const short types[] = {F_RDLCK, F_WRLCK, F_UNLCK};
    if (type < 0 || type > 2 || whence < 0 || whence > 2)
        return Fail(ctx, EINVAL);
    struct flock lock{};
    lock.l_type = types[type];
    lock.l_whence = whence;
    lock.l_start = read(start);
    lock.l_len = read(start + length);
    PosixResult(ctx, fcntl(fd, operation, &lock));
    if (!ctx.ret0 && operation == F_GETLK) {
        const auto result_type = lock.l_type == F_RDLCK ? 0 : lock.l_type == F_WRLCK ? 1 : 2;
        mem.Write<int16_t>(pointer, result_type);
        if (result_type != 2) {
            mem.Write<int16_t>(pointer + 2, lock.l_whence);
            if (wide) {
                mem.Write<int64_t>(pointer + start, lock.l_start);
                mem.Write<int64_t>(pointer + start + length, lock.l_len);
            } else {
                if (lock.l_start > INT32_MAX || lock.l_len > INT32_MAX)
                    return Fail(ctx, EOVERFLOW);
                mem.Write<int32_t>(pointer + start, lock.l_start);
                mem.Write<int32_t>(pointer + start + length, lock.l_len);
            }
            mem.Write<int32_t>(pointer + start + 2 * length, lock.l_pid);
        }
    }
}

void Fcntl(wa_host_call_ctx_t& ctx) {
    auto& dbt = *ctx.dbt;
    const auto fd = HostFd(dbt, ctx.args[0]);
    if (fd < 0)
        return PosixResult(ctx, -1);
    const auto value = static_cast<int>(ctx.args[2]);
    switch (ctx.args[1]) {
    case 0:
    case 1030: {
        if (value < 0)
            return Fail(ctx, EINVAL);
        const auto copy =
            AdoptFd(dbt, fcntl(fd, ctx.args[1] ? F_DUPFD_CLOEXEC : F_DUPFD, 0), true, value);
        if (copy >= 0)
            dbt.bionic->descriptors.at(copy).event = EventFor(dbt, ctx.args[0]);
        PosixResult(ctx, copy);
        break;
    }
    case 1:
        PosixResult(ctx, fcntl(fd, F_GETFD));
        break;
    case 2:
        PosixResult(ctx, fcntl(fd, F_SETFD, value & 1));
        break;
    case 3: {
        const auto flags = fcntl(fd, F_GETFL);
        auto guest = flags < 0 ? flags : GuestOpenFlags(flags);
        if (auto event = EventFor(dbt, ctx.args[0]); event && flags >= 0)
            guest = (guest & ~00004003) | O_RDWR | (event->nonblocking ? 00004000 : 0);
        PosixResult(ctx, guest);
        break;
    }
    case 4:
        if (auto event = EventFor(dbt, ctx.args[0]))
            event->nonblocking = (value & 00004000) != 0;
        else
            PosixResult(ctx, fcntl(fd, F_SETFL, OpenFlags(value)));
        break;
    case 5:
    case 12:
        return FcntlLock(ctx, fd, F_GETLK, ctx.args[1] == 12);
    case 6:
    case 13:
        return FcntlLock(ctx, fd, F_SETLK, ctx.args[1] == 13);
    case 7:
    case 14:
        return FcntlLock(ctx, fd, F_SETLKW, ctx.args[1] == 14);
    default:
        Fail(ctx, EINVAL);
        break;
    }
}

void Ioctl(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    const auto fd = HostFd(*ctx.dbt, ctx.args[0]);
    switch (ctx.args[1]) {
    case 0x541b: { // FIONREAD
        int value = 0;
        if (ioctl(fd, FIONREAD, &value))
            return PosixResult(ctx, -1);
        mem.Write<int32_t>(ctx.args[2], value);
        break;
    }
    case 0x5421: { // FIONBIO
        int value = mem.Read<int32_t>(ctx.args[2]);
        PosixResult(ctx, ioctl(fd, FIONBIO, &value));
        break;
    }
    case 0x5413: { // TIOCGWINSZ
        winsize size{};
        if (ioctl(fd, TIOCGWINSZ, &size))
            return PosixResult(ctx, -1);
        const uint16_t layout[]{size.ws_row, size.ws_col, size.ws_xpixel, size.ws_ypixel};
        mem.Write(ctx.args[2], layout, sizeof(layout));
        break;
    }
    default:
        Fail(ctx, ENOTTY);
        break;
    }
}

template <bool Flags> void Pipe(wa_host_call_ctx_t& ctx) {
    const auto guest_flags = Flags ? ctx.args[1] : 0;
    if (guest_flags & ~(UINT64_C(02000000) | 00004000))
        return Fail(ctx, EINVAL);
    int fds[2];
    if (pipe(fds))
        return PosixResult(ctx, -1);
    for (int fd : fds) {
        if (guest_flags & 02000000)
            fcntl(fd, F_SETFD, FD_CLOEXEC);
        if (guest_flags & 00004000)
            fcntl(fd, F_SETFL, O_NONBLOCK);
    }
    StorePipe(ctx, fds[0], fds[1], ctx.args[0]);
}

constexpr Flag kPollFlags[] = {
    {0x001, POLLIN},     {0x002, POLLPRI},    {0x004, POLLOUT},    {0x008, POLLERR},
    {0x010, POLLHUP},    {0x020, POLLNVAL},   {0x040, POLLRDNORM}, {0x080, POLLRDBAND},
    {0x100, POLLWRNORM}, {0x200, POLLWRBAND},
};

int PollEvents(int flags, bool to_host) {
    int result = 0;
    for (auto entry : kPollFlags) {
        if (flags & (to_host ? entry.guest : entry.host))
            result |= to_host ? entry.host : entry.guest;
    }
    return result;
}

int PollWait(wa_host_call_ctx_t& ctx, std::vector<pollfd>& descriptors, int millis) {
    using namespace std::chrono;
    auto& dbt = *ctx.dbt;
    auto& thread = *dbt.running;
    if (thread.io_deadline == Deadline{}) {
        thread.io_deadline = millis < 0 ? kNoDeadline : Clock::now() + milliseconds(millis);
    }
    while (true) {
        const auto result = poll(descriptors.data(), descriptors.size(), 0);
        const auto now = Clock::now();
        if (result != 0 || now >= thread.io_deadline) {
            thread.io_deadline = {};
            thread.wait = {};
            return result;
        }
        const auto wake = std::min(thread.io_deadline, now + milliseconds(2));
        Park(dbt, WaitKind::Io, 0, wake);
    }
}

void Poll(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    const auto count = ctx.args[1];
    if (count > kIovLimit)
        return Fail(ctx, EINVAL);
    std::vector<pollfd> fds(count);
    std::vector<bool> invalid(count);
    int missing = 0;
    for (size_t i = 0; i < count; ++i) {
        const auto address = ctx.args[0] + i * 8;
        const auto guest_fd = mem.Read<int32_t>(address);
        fds[i].fd = guest_fd < 0 ? -1 : HostFd(*ctx.dbt, guest_fd);
        invalid[i] = guest_fd >= 0 && fds[i].fd < 0;
        missing += invalid[i];
        fds[i].events = PollEvents(mem.Read<int16_t>(address + 4), true);
    }
    const auto result = PollWait(ctx, fds, missing ? 0 : static_cast<int32_t>(ctx.args[2]));
    PosixResult(ctx, result < 0 ? result : result + missing);
    if (result >= 0)
        for (size_t i = 0; i < count; ++i)
            mem.Write<int16_t>(ctx.args[0] + i * 8 + 6,
                               invalid[i] ? 0x20 : PollEvents(fds[i].revents, false));
}

template <int Operation> void FdSet(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    const auto fd = ctx.args[0];
    if (fd >= 1024 || ctx.args[2] < 128)
        throw GuestFault{WA_ERR_FAULT};
    const auto address = ctx.args[1] + (fd / 32) * 4;
    const auto bit = uint32_t{1} << (fd % 32);
    const auto value = mem.Read<uint32_t>(address);
    if constexpr (Operation == 0)
        ctx.ret0 = (value & bit) != 0;
    else
        mem.Write<uint32_t>(address, Operation == 1 ? value | bit : value & ~bit);
}

void Select(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    CallReader args(ctx);
    const auto nfds = args.Next();
    const uint64_t output[3]{args.Next(), args.Next(), args.Next()};
    const auto timeout = args.Next();
    if (nfds > 1024)
        return Fail(ctx, EINVAL);
    std::array<std::array<uint32_t, 32>, 3> bits{};
    for (size_t i = 0; i < 3; ++i)
        if (output[i])
            mem.Read(output[i], bits[i].data(), 128);
    std::vector<pollfd> descriptors;
    std::vector<uint32_t> guest_descriptors;
    constexpr short events[] = {POLLIN, POLLOUT, POLLPRI};
    for (uint32_t fd = 0; fd < nfds; ++fd) {
        short mask = 0;
        for (size_t i = 0; i < 3; ++i)
            if (bits[i][fd / 32] & (1U << (fd % 32)))
                mask |= events[i];
        if (mask) {
            const auto host = HostFd(*ctx.dbt, fd);
            if (host < 0)
                return PosixResult(ctx, -1);
            descriptors.push_back({host, mask, 0});
            guest_descriptors.push_back(fd);
        }
    }
    int millis = -1;
    if (timeout) {
        const auto sec = SignedWord(mem, mem.ReadWord(timeout));
        const auto usec = SignedWord(mem, mem.ReadWord(timeout + mem.width()));
        if (sec < 0 || usec < 0 || usec >= 1000000)
            return Fail(ctx, EINVAL);
        millis = sec >= INT_MAX / 1000 ? INT_MAX : sec * 1000 + (usec + 999) / 1000;
    }
    if (PollWait(ctx, descriptors, millis) < 0)
        return PosixResult(ctx, -1);
    bits = {};
    for (size_t index = 0; index < descriptors.size(); ++index) {
        const auto& fd = descriptors[index];
        const auto guest = guest_descriptors[index];
        if (fd.revents & POLLNVAL)
            return Fail(ctx, EBADF);
        for (size_t i = 0; i < 3; ++i) {
            if (output[i] && ((fd.revents & events[i]) || (i < 2 && (fd.events & events[i]) &&
                                                           (fd.revents & (POLLERR | POLLHUP))))) {
                bits[i][guest / 32] |= 1U << (guest % 32);
                ++ctx.ret0;
            }
        }
    }
    for (size_t i = 0; i < 3; ++i)
        if (output[i])
            mem.Write(output[i], bits[i].data(), 128);
    if (timeout && !ctx.ret0) {
        mem.WriteWord(timeout, 0);
        mem.WriteWord(timeout + mem.width(), 0);
    }
}

void Fchmod(wa_host_call_ctx_t& ctx) {
    PosixResult(ctx, fchmod(HostFd(*ctx.dbt, ctx.args[0]), ctx.args[1]));
}
void Fchown(wa_host_call_ctx_t& ctx) {
    PosixResult(ctx, fchown(HostFd(*ctx.dbt, ctx.args[0]), ctx.args[1], ctx.args[2]));
}

void Fdatasync(wa_host_call_ctx_t& ctx) {
#if defined(__APPLE__)
    PosixResult(ctx, fsync(HostFd(*ctx.dbt, ctx.args[0])));
#else
    PosixResult(ctx, fdatasync(HostFd(*ctx.dbt, ctx.args[0])));
#endif
}

template <bool Allocate, bool Wide = false> void PosixAllocation(wa_host_call_ctx_t& ctx) {
    CallReader args(ctx);
    const auto fd = HostFd(*ctx.dbt, args.Next());
    const GuestMemory mem(ctx);
    const auto offset =
        Wide ? static_cast<int64_t>(args.Next(ArgKind::Long)) : SignedWord(mem, args.Next());
    const auto length =
        Wide ? static_cast<int64_t>(args.Next(ArgKind::Long)) : SignedWord(mem, args.Next());
    if (offset < 0 || length < 0 || length > INT64_MAX - offset || (Allocate && !length)) {
        ctx.ret0 = GuestErrno(EINVAL);
        return;
    }
#if defined(__APPLE__)
    if constexpr (Allocate) {
        struct stat info{};
        if (fstat(fd, &info)) {
            ctx.ret0 = GuestErrno(errno);
            return;
        }
        const auto end = offset + length;
        if (end > info.st_size) {
            fstore_t request{F_ALLOCATEALL, F_PEOFPOSMODE, 0, end - info.st_size, 0};
            if (fcntl(fd, F_PREALLOCATE, &request) || ftruncate(fd, end))
                ctx.ret0 = GuestErrno(errno);
        }
    } else {
        const auto advice = args.Next();
        if (advice > 5)
            ctx.ret0 = GuestErrno(EINVAL);
        else if (fcntl(fd, F_GETFD) < 0)
            ctx.ret0 = GuestErrno(errno);
    }
#else
    if constexpr (Allocate)
        ctx.ret0 = GuestErrno(posix_fallocate(fd, offset, length));
    else
        ctx.ret0 = GuestErrno(posix_fadvise(fd, offset, length, args.Next()));
#endif
}

void Sendfile(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    const auto out = HostFd(*ctx.dbt, ctx.args[0]), in = HostFd(*ctx.dbt, ctx.args[1]);
    if (out < 0 || in < 0)
        return PosixResult(ctx, -1);
    const auto offset_address = ctx.args[2];
    const auto limit = std::min<uint64_t>(ctx.args[3], kIoLimit);
    std::vector<char> bytes(limit);
    const auto offset = offset_address ? SignedWord(mem, mem.ReadWord(offset_address)) : 0;
    const auto count = offset_address ? pread(in, bytes.data(), bytes.size(), offset)
                                      : read(in, bytes.data(), bytes.size());
    if (count <= 0)
        return PosixResult(ctx, count);
    const auto result = write(out, bytes.data(), count);
    PosixResult(ctx, result);
    if (offset_address && result > 0)
        mem.WriteWord(offset_address, offset + result);
    if (!offset_address && result != count)
        lseek(in, std::max<ssize_t>(result, 0) - count, SEEK_CUR);
}

void MountTable(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    // /proc/mounts is not a host ABI. Provide the guest's own root entry.
    const auto path = mem.String(ctx.args[0]);
    if (path != "/proc/mounts" && path != "/proc/self/mounts" && path != "/etc/mtab") {
        SetErrno(*ctx.dbt, GuestErrno(ENOENT));
        return;
    }
    uint64_t result = 0;
    const auto tmpfile = FindVirtualSymbol(*ctx.dbt, "libc.so", "tmpfile");
    CheckGuest(wa_dbt_call(ctx.dbt, tmpfile, nullptr, 0, &result));
    if (!result)
        return;
    auto& file = ctx.dbt->bionic->files.at(result);
    std::fputs("wasmandroid / tmpfs rw 0 0\n", file.host);
    std::rewind(file.host);
    ctx.ret0 = result;
}

void ReadMount(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    const auto it = ctx.dbt->bionic->files.find(ctx.args[0]);
    if (it == ctx.dbt->bionic->files.end()) {
        SetErrno(*ctx.dbt, GuestErrno(EBADF));
        return;
    }
    constexpr char fields[] = "wasmandroid\0/\0tmpfs\0rw";
    if (ctx.args[3] < sizeof(fields)) {
        SetErrno(*ctx.dbt, GuestErrno(ERANGE));
        return;
    }
    if (std::fgetc(it->second.host) == EOF)
        return;
    while (const auto c = std::fgetc(it->second.host))
        if (c == '\n' || c == EOF)
            break;
    mem.Write(ctx.args[2], fields, sizeof(fields));
    size_t offset = 0;
    for (size_t i = 0; i < 4; ++i) {
        mem.WriteWord(ctx.args[1] + i * mem.width(), ctx.args[2] + offset);
        offset += std::strlen(fields + offset) + 1;
    }
    mem.Write<uint64_t>(ctx.args[1] + 4 * mem.width(), 0);
    ctx.ret0 = ctx.args[1];
}

void CloseMount(wa_host_call_ctx_t& ctx) {
    const auto fclose = FindVirtualSymbol(*ctx.dbt, "libc.so", "fclose");
    uint64_t result;
    CheckGuest(wa_dbt_call(ctx.dbt, fclose, ctx.args, 1, &result));
    ctx.ret0 = 1;
}

void Eventfd(wa_host_call_ctx_t& ctx) {
    auto& dbt = *ctx.dbt;
    const auto flags = ctx.args[1];
    if (flags & ~UINT64_C(0x80801))
        return Fail(ctx, EINVAL);
    auto event = std::make_shared<GuestEvent>();
    int pipe_fds[2];
    if (pipe(pipe_fds))
        return PosixResult(ctx, -1);
    event->notification = pipe_fds[1];
    event->count = static_cast<uint32_t>(ctx.args[0]);
    event->id = dbt.bionic->next_event++;
    event->semaphore = (flags & 1) != 0;
    event->nonblocking = (flags & 0x800) != 0;
    for (const auto fd : pipe_fds) {
        fcntl(fd, F_SETFL, O_NONBLOCK);
        if (flags & 0x80000)
            fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    if (event->count) {
        const char token = 1;
        if (write(event->notification, &token, 1) != 1) {
            const auto error = errno;
            close(pipe_fds[0]);
            return Fail(ctx, error);
        }
    }
    const auto guest = AdoptFd(dbt, pipe_fds[0]);
    if (guest >= 0)
        dbt.bionic->descriptors.at(guest).event = std::move(event);
    PosixResult(ctx, guest);
}

void Isatty(wa_host_call_ctx_t& ctx) {
    ctx.ret0 = isatty(HostFd(*ctx.dbt, ctx.args[0]));
    if (!ctx.ret0)
        SetErrno(*ctx.dbt, GuestErrno(errno));
}

} // namespace

std::string GuestPath(const wa_host_call_ctx_t& ctx, uint64_t address) {
    return Path(ctx, address);
}

void RegisterFiles(wa_dbt& dbt) {
#define PATH(name, op) {#name, Dispatch<PathCall<FilePath::op>>}
#define DIR(name, op) {#name, Dispatch<DirectoryCall<Directory::op>>}
#define FD(name)                                                                                   \
    {                                                                                              \
        #name, Dispatch<FdCall<::name>>                                                            \
    }
    const Builtin c[] = {
        {"open", Dispatch<OpenFile<Open::Plain>>},
        {"open64", Dispatch<OpenFile<Open::Plain>>},
        {"__open_2", Dispatch<OpenFile<Open::Checked>>},
        {"openat", Dispatch<OpenFile<Open::At>>},
        {"openat64", Dispatch<OpenFile<Open::At>>},
        {"__openat_2", Dispatch<OpenFile<Open::AtChecked>>},
        {"close", Dispatch<Close>},
        {"dup", Dispatch<DuplicateFd<false>>},
        {"dup2", Dispatch<DuplicateFd<true>>},
        FD(fsync),
        FD(flock),
        {"isatty", Dispatch<Isatty>},
        {"read", Dispatch<Transfer<Io::Read>>},
        {"__read_chk", Dispatch<Transfer<Io::ReadChecked>>},
        {"write", Dispatch<Transfer<Io::Write>>},
        {"__write_chk", Dispatch<Transfer<Io::WriteChecked>>},
        {"pread", Dispatch<Transfer<Io::Pread>>},
        {"pread64", Dispatch<Transfer<Io::Pread64>>},
        {"pwrite", Dispatch<Transfer<Io::Pwrite>>},
        {"pwrite64", Dispatch<Transfer<Io::Pwrite64>>},
        {"readv", Dispatch<VectorTransfer<true>>},
        {"writev", Dispatch<VectorTransfer<false>>},
        {"lseek", Dispatch<OffsetCall<false, false>>},
        {"lseek64", Dispatch<OffsetCall<true, false>>},
        {"ftruncate", Dispatch<OffsetCall<false, true>>},
        {"ftruncate64", Dispatch<OffsetCall<true, true>>},
        {"fstat", Dispatch<StatCall<Stat::Descriptor>>},
        {"fstat64", Dispatch<StatCall<Stat::Descriptor>>},
        {"stat", Dispatch<StatCall<Stat::File>>},
        {"stat64", Dispatch<StatCall<Stat::File>>},
        {"lstat", Dispatch<StatCall<Stat::Link>>},
        {"lstat64", Dispatch<StatCall<Stat::Link>>},
        {"fstatat", Dispatch<StatCall<Stat::At>>},
        {"fstatat64", Dispatch<StatCall<Stat::At>>},
        {"statvfs", Dispatch<Statvfs>},
        {"fstatfs", Dispatch<Statfs<false>>},
        {"statfs", Dispatch<Statfs<true>>},
        PATH(access, Access),
        PATH(unlink, Unlink),
        PATH(mkdir, Mkdir),
        PATH(rmdir, Rmdir),
        PATH(rename, Rename),
        PATH(remove, Remove),
        PATH(link, Link),
        PATH(symlink, Symlink),
        PATH(chmod, Chmod),
        PATH(fchmodat, ChmodAt),
        PATH(unlinkat, UnlinkAt),
        PATH(truncate, Truncate),
        PATH(truncate64, Truncate64),
        PATH(readlink, Readlink),
        PATH(realpath, Realpath),
        PATH(chdir, Chdir),
        PATH(pathconf, Pathconf),
        PATH(utime, Utime),
        PATH(utimes, Utimes),
        PATH(utimensat, UtimensAt),
        {"getcwd", Dispatch<Getcwd>},
        {"mkstemp", Dispatch<Mkstemp>},
        {"mkstemp64", Dispatch<Mkstemp>},
        {"mkdtemp", Dispatch<Mkdtemp>},
        {"nftw", Dispatch<WalkFiles>},
        {"tempnam", Dispatch<Tempnam>},
        DIR(opendir, Open),
        DIR(fdopendir, FdOpen),
        DIR(readdir, Read),
        DIR(readdir64, Read),
        DIR(readdir_r, Reentrant),
        DIR(closedir, Close),
        DIR(rewinddir, Rewind),
        DIR(dirfd, Descriptor),
        {"fcntl", Dispatch<Fcntl>},
        {"ioctl", Dispatch<Ioctl>},
        {"pipe", Dispatch<Pipe<false>>},
        {"pipe2", Dispatch<Pipe<true>>},
        {"poll", Dispatch<Poll>},
        {"select", Dispatch<Select>},
        {"__FD_ISSET_chk", Dispatch<FdSet<0>>},
        {"__FD_SET_chk", Dispatch<FdSet<1>>},
        {"__FD_CLR_chk", Dispatch<FdSet<2>>},
        {"fchmod", Dispatch<Fchmod>},
        {"fchown", Dispatch<Fchown>},
        {"fdatasync", Dispatch<Fdatasync>},
        {"posix_fallocate", Dispatch<PosixAllocation<true>>},
        {"posix_fadvise", Dispatch<PosixAllocation<false>>},
        {"posix_fadvise64", Dispatch<PosixAllocation<false, true>>},
        {"posix_fallocate64", Dispatch<PosixAllocation<true, true>>},
        {"sendfile", Dispatch<Sendfile>},
        {"setmntent", Dispatch<MountTable>},
        {"getmntent_r", Dispatch<ReadMount>},
        {"endmntent", Dispatch<CloseMount>},
        {"eventfd", Dispatch<Eventfd>},
    };
#undef FD
#undef DIR
#undef PATH
    Register(dbt, "libc.so", c);
    RegisterMappings(dbt);
}

} // namespace wa::dbt
