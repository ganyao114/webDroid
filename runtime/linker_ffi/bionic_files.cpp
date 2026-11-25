#include "bionic_internal.h"
#include "elf_image.h"
#include "softmmu_internal.h"

#include <cerrno>
#include <climits>
#include <exception>
#include <fcntl.h>
#include <fnmatch.h>
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

#if !defined(__EMSCRIPTEN__)
#include <fts.h>
#endif

#if defined(__linux__) && !defined(__EMSCRIPTEN__)
#include <sys/inotify.h>
#endif

#if defined(__APPLE__)
#include <sys/mount.h>
#else
#include <sys/statfs.h>
#endif

namespace wa::dbt {

#if !defined(__EMSCRIPTEN__)
struct GuestFileTree {
    explicit GuestFileTree(wa_dbt& dbt) : handle(dbt, 1) {}
    ~GuestFileTree() { if (host) fts_close(host); }

    GuestBuffer handle;
    FTS* host = nullptr;
    FTSENT* current = nullptr;
    uint64_t entry = 0;
    int options = 0;
    std::vector<std::string> roots;
    std::vector<std::unique_ptr<GuestBuffer>> snapshot;
};
#endif

std::optional<GuestMountedFile> FindMountedFile(const wa_dbt& dbt, std::string_view path) {
    const auto& find = dbt.bionic->find_guest_file;
    return find && path.starts_with('/') ? find(path) : std::nullopt;
}

int OpenMountedFile(const GuestMountedFile& file, int flags) {
    if ((flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC))) {
        errno = EROFS;
        return -1;
    }
    if (flags & O_DIRECTORY) {
        errno = file.directory ? ENOTSUP : ENOTDIR;
        return -1;
    }
    if (file.directory) {
        errno = EISDIR;
        return -1;
    }
    // Give Bionic's descriptor operations a private, seekable snapshot of data
    // from the VM filesystem. File-backed APK entries use the ELF reader.
    char path[] = "/tmp/wa-native-file-XXXXXX";
    const int backing = mkstemp(path);
    if (backing < 0) return -1;
    struct CloseBacking {
        int fd;
        const char* path;
        ~CloseBacking() {
            const int error = errno;
            close(fd);
            unlink(path);
            errno = error;
        }
    } cleanup{backing, path};
    std::array<uint8_t, 16 * 1024> bytes;
    for (uint64_t offset = 0; offset < file.size;) {
        const auto limit = std::min<uint64_t>(bytes.size(), file.size - offset);
        const auto count = file.read(bytes.data(), limit, offset);
        if (count <= 0 || static_cast<uint64_t>(count) > limit) {
            errno = EIO;
            return -1;
        }
        for (int64_t written = 0; written < count;) {
            const auto result = write(backing, bytes.data() + written, count - written);
            if (result < 0 && errno == EINTR) continue;
            if (result <= 0) {
                if (result == 0) errno = EIO;
                return -1;
            }
            written += result;
        }
        offset += count;
    }
    return open(path, flags);
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

bool StatMountedFile(const wa_host_call_ctx_t& ctx, std::string_view path, struct stat& result) {
    const auto data = FindMountedFile(*ctx.dbt, path);
    if (!data) return false;
    result.st_mode = data->directory ? S_IFDIR | 0555 : S_IFREG | 0444;
    result.st_nlink = 1;
    result.st_size = data->size;
    result.st_blksize = WA_PAGE_SIZE;
    result.st_blocks = (data->size + 511) / 512;
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
    const auto mounted = FindMountedFile(*ctx.dbt, path);
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
    const bool timer = event->timer_clock >= 0;
    if (count < sizeof(uint64_t) || (timer && !read)) {
        Fail(ctx, EINVAL);
        return true;
    }
    const auto wait = [&](WaitKind kind) {
        if (event->nonblocking) {
            Fail(ctx, EAGAIN);
            return false;
        }
        const auto deadline = timer ? Clock::now() + std::chrono::milliseconds(2) : kNoDeadline;
        if (const auto error = Park(dbt, kind, event->id, deadline);
                error && !(timer && error == GuestErrno(ETIMEDOUT))) {
            SetErrno(dbt, error);
            ctx.ret0 = UINT64_MAX;
            return false;
        }
        return true;
    };
    if (read) {
        for (;;) {
            if (timer) RefreshTimers(dbt);
            if (event->count) break;
            if (!wait(WaitKind::EventRead))
                return true;
        }
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

enum class Io { Read, Write, Pread, Pwrite, Pread64, Pwrite64 };
template <Io op, bool Checked = false> void Transfer(wa_host_call_ctx_t& ctx) {
    constexpr bool read = op == Io::Read || op == Io::Pread || op == Io::Pread64;
    constexpr bool positioned = op != Io::Read && op != Io::Write;
    constexpr bool wide = op == Io::Pread64 || op == Io::Pwrite64;
    const GuestMemory mem(ctx);
    CallReader args(ctx);
    const auto descriptor = static_cast<int>(args.Next());
    const auto fd = HostFd(*ctx.dbt, descriptor);
    if (fd < 0)
        return PosixResult(ctx, -1);
    const auto address = args.Next(), count = args.Next();
    const auto offset = positioned ? (wide ? static_cast<int64_t>(args.Next(ArgKind::Long))
                                           : SignedWord(mem, args.Next())) : 0;
    if constexpr (Checked)
        if (count > args.Next() || count > (mem.width() == 8 ? INT64_MAX : INT32_MAX))
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

#if !defined(__EMSCRIPTEN__)
uint64_t FileTreeEntry(wa_host_call_ctx_t& ctx, GuestFileTree& tree, const FTSENT& entry,
                       std::map<const FTSENT*, uint64_t>& ancestors) {
    const auto parent = entry.fts_parent && entry.fts_level >= 0
        ? FileTreeEntry(ctx, tree, *entry.fts_parent, ancestors) : 0;
    const GuestMemory mem(ctx);
    const auto word = mem.width();
    const auto numeric = 7 * word;
    const auto inode = numeric + 8 + 2 * word;
    const auto level = inode + 2 * word + 4;
    const auto stat_ptr = AlignUp(level + 4 * sizeof(uint16_t), word);
    const auto name = stat_ptr + word;
    const auto path = name + entry.fts_namelen + 1;
    const auto stat = AlignUp(path + entry.fts_pathlen + 1, 8);
    auto buffer = std::make_unique<GuestBuffer>(*ctx.dbt, stat + 128);
    const auto address = buffer->address;
    tree.snapshot.push_back(std::move(buffer));
    ancestors.emplace(&entry, address);
    const auto cycle = entry.fts_info == FTS_DC ? ancestors.find(entry.fts_cycle) : ancestors.end();
    mem.WriteWord(address, cycle == ancestors.end() ? 0 : cycle->second);
    mem.WriteWord(address + word, parent);
    mem.WriteWord(address + 3 * word, entry.fts_number);
    mem.WriteWord(address + 4 * word, reinterpret_cast<uintptr_t>(entry.fts_pointer));
    mem.WriteWord(address + 5 * word, address + path);
    mem.WriteWord(address + 6 * word, address + path);
    mem.Write<int32_t>(address + numeric, GuestErrno(entry.fts_errno));
    mem.Write<int32_t>(address + numeric + 4, -1);
    mem.WriteWord(address + numeric + 8, entry.fts_pathlen);
    mem.WriteWord(address + numeric + 8 + word, entry.fts_namelen);
    mem.WriteWord(address + inode, entry.fts_ino);
    mem.WriteWord(address + inode + word, entry.fts_dev);
    mem.Write<uint32_t>(address + inode + 2 * word, entry.fts_nlink);
    mem.Write<int16_t>(address + level, entry.fts_level);
    mem.Write<uint16_t>(address + level + 2, entry.fts_info);
    mem.Write<uint16_t>(address + level + 4, entry.fts_flags);
    mem.Write<uint16_t>(address + level + 6, entry.fts_instr);
    mem.Write(address + name, entry.fts_name, entry.fts_namelen);
    mem.Write<uint8_t>(address + name + entry.fts_namelen, 0);
    mem.Write(address + path, entry.fts_path, entry.fts_pathlen);
    mem.Write<uint8_t>(address + path + entry.fts_pathlen, 0);
    if (entry.fts_level >= 0 && entry.fts_info != FTS_NS && entry.fts_statp && !(tree.options & FTS_NOSTAT)) {
        WriteStat(mem, address + stat, *entry.fts_statp);
        mem.WriteWord(address + stat_ptr, address + stat);
    }
    return address;
}

enum class Walk { Open, Read, Set, Close };
template<Walk operation> void FileTree(wa_host_call_ctx_t& ctx) {
    auto& dbt = *ctx.dbt;
    auto& trees = dbt.bionic->file_trees;
    const GuestMemory mem(ctx);
    if constexpr (operation == Walk::Open) {
        if (!ctx.args[0] || (ctx.args[1] & ~0x7fu) || ctx.args[2]) {
            SetErrno(dbt, GuestErrno(ctx.args[2] ? ENOTSUP : EINVAL));
            return;
        }
        auto tree = std::make_shared<GuestFileTree>(dbt);
        tree->options = static_cast<int>(ctx.args[1]) | FTS_NOCHDIR;
        for (auto slot = ctx.args[0];; slot += mem.width()) {
            const auto path = mem.ReadWord(slot);
            if (!path) break;
            tree->roots.push_back(Path(ctx, path));
        }
        std::vector<char*> roots;
        for (auto& path : tree->roots) roots.push_back(path.data());
        roots.push_back(nullptr);
        tree->host = fts_open(roots.data(), tree->options, nullptr);
        if (!tree->host) {
            SetErrno(dbt, GuestErrno(errno));
            return;
        }
        const auto handle = tree->handle.address;
        trees.emplace(handle, std::move(tree));
        ctx.ret0 = handle;
    } else {
        const auto found = trees.find(ctx.args[0]);
        if (found == trees.end()) {
            SetErrno(dbt, GuestErrno(EINVAL));
            if constexpr (operation != Walk::Read) ctx.ret0 = UINT64_MAX;
            return;
        }
        auto& tree = *found->second;
        if constexpr (operation == Walk::Close) {
            const auto result = fts_close(std::exchange(tree.host, nullptr));
            const auto error = errno;
            trees.erase(found);
            errno = error;
            PosixResult(ctx, result);
        } else if constexpr (operation == Walk::Set) {
            if (!tree.current || ctx.args[1] != tree.entry) return Fail(ctx, EINVAL);
            PosixResult(ctx, fts_set(tree.host, tree.current, static_cast<int>(ctx.args[2])));
        } else {
            if (tree.entry && tree.current) {
                tree.current->fts_number = SignedWord(mem, mem.ReadWord(tree.entry + 3 * mem.width()));
                tree.current->fts_pointer = reinterpret_cast<void*>(
                    static_cast<uintptr_t>(mem.ReadWord(tree.entry + 4 * mem.width())));
            }
            tree.snapshot.clear();
            tree.entry = 0;
            errno = 0;
            tree.current = fts_read(tree.host);
            if (!tree.current) {
                SetErrno(dbt, GuestErrno(errno));
                return;
            }
            std::map<const FTSENT*, uint64_t> ancestors;
            ctx.ret0 = tree.entry = FileTreeEntry(ctx, tree, *tree.current, ancestors);
        }
    }
}
#endif

void FilenameMatch(wa_host_call_ctx_t& ctx) {
    constexpr Flag flags[] = {
        {0x01, FNM_NOESCAPE}, {0x02, FNM_PATHNAME}, {0x04, FNM_PERIOD},
        {0x08, FNM_LEADING_DIR}, {0x10, FNM_CASEFOLD},
    };
    int options = 0;
    for (const auto flag : flags)
        if (ctx.args[2] & flag.guest) options |= flag.host;
    const GuestMemory mem(ctx);
    const auto pattern = mem.String(ctx.args[0]);
    const auto path = mem.String(ctx.args[1]);
    ctx.ret0 = fnmatch(pattern.c_str(), path.c_str(), options) == 0 ? 0 : 1;
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
    AccessAt,
    Unlink,
    Mkdir,
    MkdirAt,
    Mkfifo,
    Mknod,
    Rmdir,
    Remove,
    Rename,
    RenameAt,
    Link,
    Symlink,
    SymlinkAt,
    Chmod,
    UnlinkAt,
    ChmodAt,
    Truncate,
    Truncate64,
    Readlink,
    ReadlinkAt,
    Realpath,
    Chdir,
    Pathconf,
    Utime,
    Utimes,
    UtimensAt
};
template <FilePath op, bool Checked = false> void PathCall(wa_host_call_ctx_t& ctx) {
    constexpr bool at = op == FilePath::AccessAt || op == FilePath::MkdirAt || op == FilePath::RenameAt ||
        op == FilePath::ReadlinkAt || op == FilePath::UnlinkAt || op == FilePath::ChmodAt || op == FilePath::UtimensAt;
    const GuestMemory mem(ctx);
    const auto fd = at ? AtFd(ctx, ctx.args[0]) : AT_FDCWD;
    const auto path = Path(ctx, ctx.args[at ? 1 : 0], fd);
    const auto p = path.c_str();
    int64_t result = 0;
    if constexpr (op == FilePath::Access || op == FilePath::AccessAt) {
        const auto mode = ctx.args[at ? 2 : 1];
        if (mode & ~(R_OK | W_OK | X_OK)) return Fail(ctx, EINVAL);
        const auto flags = at ? ctx.args[3] : 0;
        if (flags & ~0x300u) return Fail(ctx, EINVAL);
        if (const auto mounted = FindMountedFile(*ctx.dbt, path)) {
            if (mode & (mounted->directory ? W_OK : W_OK | X_OK)) return Fail(ctx, EACCES);
            return;
        }
        if constexpr (at) {
            result = faccessat(fd, p, mode, (flags & 0x100 ? AT_SYMLINK_NOFOLLOW : 0) |
                                             (flags & 0x200 ? AT_EACCESS : 0));
        } else {
            result = access(p, mode);
        }
    }
    if constexpr (op == FilePath::Unlink)
        result = unlink(p);
    if constexpr (op == FilePath::Mkdir)
        result = mkdir(p, ctx.args[1]);
    if constexpr (op == FilePath::MkdirAt)
        result = mkdirat(fd, p, ctx.args[2]);
    if constexpr (op == FilePath::Mkfifo || op == FilePath::Mknod) {
        const auto mode = static_cast<mode_t>(ctx.args[1]);
        if constexpr (op == FilePath::Mknod) {
            if (!S_ISFIFO(mode)) return Fail(ctx, EPERM);
        }
        if (FindMountedFile(*ctx.dbt, path)) return Fail(ctx, EROFS);
        result = mkfifo(p, mode & 07777);
    }
    if constexpr (op == FilePath::Rmdir)
        result = rmdir(p);
    if constexpr (op == FilePath::Remove)
        result = std::remove(p);
    if constexpr (op == FilePath::Rename)
        result = std::rename(p, Path(ctx, ctx.args[1]).c_str());
    if constexpr (op == FilePath::RenameAt) {
        const auto new_fd = AtFd(ctx, ctx.args[2]);
        result = renameat(fd, p, new_fd, Path(ctx, ctx.args[3], new_fd).c_str());
    }
    if constexpr (op == FilePath::Link)
        result = link(p, Path(ctx, ctx.args[1]).c_str());
    if constexpr (op == FilePath::Symlink)
        result = symlink(mem.String(ctx.args[0]).c_str(), Path(ctx, ctx.args[1]).c_str());
    if constexpr (op == FilePath::SymlinkAt) {
        const auto new_fd = AtFd(ctx, ctx.args[1]);
        result = symlinkat(mem.String(ctx.args[0]).c_str(), new_fd, Path(ctx, ctx.args[2], new_fd).c_str());
    }
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
    if constexpr (op == FilePath::Readlink || op == FilePath::ReadlinkAt) {
        if (FindMountedFile(*ctx.dbt, path)) return Fail(ctx, EINVAL);
        const auto count = ctx.args[at ? 3 : 2];
        if constexpr (Checked)
            if (count > ctx.args[at ? 4 : 3]) throw GuestFault{WA_ERR_FAULT};
        std::vector<char> bytes(std::min<uint64_t>(count, kIoLimit));
        result = readlinkat(fd, p, bytes.data(), bytes.size());
        if (result > 0)
            mem.Write(ctx.args[at ? 2 : 1], bytes.data(), result);
    }
    if constexpr (op == FilePath::Realpath || op == FilePath::Chdir) {
        if (const auto mounted = FindMountedFile(*ctx.dbt, path)) {
            if constexpr (op == FilePath::Chdir) {
                if (!mounted->directory) return Fail(ctx, ENOTDIR);
                ctx.dbt->bionic->cwd = path;
            } else if (ctx.args[1]) {
                mem.Write(ctx.args[1], path.c_str(), path.size() + 1);
                ctx.ret0 = ctx.args[1];
            } else {
                ctx.ret0 = CopyString(*ctx.dbt, path);
            }
            return;
        }
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

template<bool Suffix> void Mkstemp(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    auto path = Path(ctx, ctx.args[0]);
    const auto length = mem.String(ctx.args[0]).size();
    const auto fd = Suffix ? mkstemps(path.data(), static_cast<int32_t>(ctx.args[1])) : mkstemp(path.data());
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

template<bool Legacy> void Tempnam(wa_host_call_ctx_t& ctx) {
    auto& dbt = *ctx.dbt;
    const GuestMemory mem(ctx);
    const auto dir = !Legacy && ctx.args[0] ? Path(ctx, ctx.args[0]) : "/tmp";
    const auto prefix = !Legacy && ctx.args[1] ? mem.String(ctx.args[1], 255, true) : "";
    auto path = dir + "/" + prefix + "XXXXXX";
    const auto fd = mkstemp(path.data());
    if (fd < 0) {
        SetErrno(dbt, GuestErrno(errno));
        return;
    }
    close(fd);
    if (unlink(path.c_str())) {
        SetErrno(dbt, GuestErrno(errno));
        return;
    }
    if constexpr (Legacy) {
        auto address = ctx.args[0];
        if (!address) {
            auto& buffer = dbt.bionic->tmpnam;
            if (!buffer) buffer = AllocateGuest(dbt, path.size() + 1);
            if (!buffer) {
                SetErrno(dbt, GuestErrno(ENOMEM));
                return;
            }
            address = buffer;
        }
        mem.Write(address, path.c_str(), path.size() + 1);
        ctx.ret0 = address;
    } else {
        ctx.ret0 = CopyString(dbt, path);
    }
}

enum class Directory { Open, FdOpen, Read, Reentrant, Close, Rewind, Descriptor };

void WriteDirectoryEntry(const GuestMemory& mem, uint64_t address, const struct dirent& item) {
    const auto length = std::strlen(item.d_name);
    if (length > 255) throw GuestFault{WA_ERR_OVERFLOW};
    mem.Write<uint64_t>(address, item.d_ino);
#if defined(__APPLE__)
    mem.Write<int64_t>(address + 8, item.d_seekoff);
#else
    mem.Write<int64_t>(address + 8, item.d_off);
#endif
    mem.Write<uint16_t>(address + 16, AlignUp(20 + length, 8));
    mem.Write<uint8_t>(address + 18, item.d_type);
    mem.Write(address + 19, item.d_name, length + 1);
}

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
                WriteDirectoryEntry(mem, output, *item);
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

void ScanDirectory(wa_host_call_ctx_t& ctx) {
    auto& dbt = *ctx.dbt;
    const GuestMemory mem(ctx);
    const auto path = Path(ctx, ctx.args[0]);
    const std::unique_ptr<DIR, decltype(&closedir)> dir(opendir(path.c_str()), closedir);
    if (!dir) return PosixResult(ctx, -1);
    std::vector<std::unique_ptr<GuestBuffer>> entries;
    for (;;) {
        errno = 0;
        const auto* item = readdir(dir.get());
        if (!item) {
            if (errno) return PosixResult(ctx, -1);
            break;
        }
        auto entry = std::make_unique<GuestBuffer>(dbt, 280);
        WriteDirectoryEntry(mem, entry->address, *item);
        if (ctx.args[2]) {
            uint64_t selected;
            CheckGuest(wa_dbt_call(&dbt, ctx.args[2], &entry->address, 1, &selected));
            if (!static_cast<int32_t>(selected)) continue;
        }
        entries.push_back(std::move(entry));
    }
    if (ctx.args[3]) {
        GuestBuffer slots(dbt, 2 * mem.width());
        const uint64_t args[]{slots.address, slots.address + mem.width()};
        std::ranges::stable_sort(entries, [&](const auto& left, const auto& right) {
            mem.WriteWord(args[0], left->address);
            mem.WriteWord(args[1], right->address);
            uint64_t order;
            CheckGuest(wa_dbt_call(&dbt, ctx.args[3], args, 2, &order));
            return static_cast<int32_t>(order) < 0;
        });
    }
    GuestBuffer list(dbt, std::max<size_t>(1, entries.size()) * mem.width());
    for (size_t i = 0; i < entries.size(); ++i)
        mem.WriteWord(list.address + i * mem.width(), entries[i]->address);
    mem.WriteWord(ctx.args[1], list.address);
    list.address = 0;
    for (auto& entry : entries) entry->address = 0;
    ctx.ret0 = entries.size();
}

void Close(wa_host_call_ctx_t& ctx) {
    PosixResult(ctx, CloseFd(*ctx.dbt, static_cast<int32_t>(ctx.args[0])));
}

template <bool Exact, bool Flags = false> void DuplicateFd(wa_host_call_ctx_t& ctx) {
    auto& dbt = *ctx.dbt;
    const auto source = static_cast<int32_t>(ctx.args[0]);
    const auto fd = HostFd(dbt, source);
    if (fd < 0)
        return PosixResult(ctx, -1);
    if constexpr (Exact) {
        const auto target = static_cast<int32_t>(ctx.args[1]);
        const auto flags = Flags ? ctx.args[2] : 0;
        if constexpr (Flags)
            if (source == target || (flags & ~uint64_t{02000000})) return Fail(ctx, EINVAL);
        if (target < 0)
            return Fail(ctx, EBADF);
        if (target != source) {
            auto& descriptors = dbt.bionic->descriptors;
            auto entry = descriptors.find(target);
            const auto epoll = EpollFor(dbt, source);
            // Never overwrite an original host standard descriptor.
            if (entry != descriptors.end() && entry->second.host >= 3 && !entry->second.event) {
                if (dup2(fd, entry->second.host) < 0)
                    return PosixResult(ctx, -1);
                entry->second.event = EventFor(dbt, source);
                entry->second.epoll = epoll;
            } else {
                const auto copy = dup(fd);
                if (copy < 0)
                    return PosixResult(ctx, -1);
                const auto event = EventFor(dbt, source);
                try {
                    if (entry != descriptors.end() && entry->second.owned)
                        close(entry->second.host);
                    descriptors.insert_or_assign(target, GuestDescriptor{copy, true, event, epoll});
                } catch (...) {
                    close(copy);
                    throw;
                }
            }
            RemoveEpollInterests(dbt, target);
            if constexpr (Flags) {
                if (flags && fcntl(HostFd(dbt, target), F_SETFD, FD_CLOEXEC) < 0)
                    return PosixResult(ctx, -1);
            }
        }
        ctx.ret0 = target;
    } else {
        const auto copy = AdoptFd(dbt, dup(fd));
        if (copy >= 0) {
            dbt.bionic->descriptors.at(copy).event = EventFor(dbt, source);
            dbt.bionic->descriptors.at(copy).epoll = EpollFor(dbt, source);
        }
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
        if (copy >= 0) {
            dbt.bionic->descriptors.at(copy).event = EventFor(dbt, ctx.args[0]);
            dbt.bionic->descriptors.at(copy).epoll = EpollFor(dbt, ctx.args[0]);
        }
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
        RefreshTimers(dbt);
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

// ARM's epoll_event has four bytes of padding before the 64-bit user data.
struct EpollEvent {
    uint32_t events, padding;
    uint64_t data;
};
static_assert(sizeof(EpollEvent) == 16 && offsetof(EpollEvent, data) == 8);
constexpr uint32_t kEpollOneShot = uint32_t{1} << 30;
constexpr uint32_t kEpollAlways = 0x018; // EPOLLERR and EPOLLHUP.
constexpr uint32_t kEpollPollFlags = 0x3df; // Linux poll flags except POLLNVAL.

template<bool Flags> void EpollCreate(wa_host_call_ctx_t& ctx) {
    const auto arg = static_cast<int32_t>(ctx.args[0]);
    if (Flags ? (arg & ~02000000) != 0 : arg <= 0) return Fail(ctx, EINVAL);
    auto epoll = std::make_shared<GuestEpoll>();
    const auto host = open("/dev/null", O_RDONLY | (Flags && (arg & 02000000) ? O_CLOEXEC : 0));
    const auto id = AdoptFd(*ctx.dbt, host);
    if (id >= 0) ctx.dbt->bionic->descriptors.at(id).epoll = std::move(epoll);
    PosixResult(ctx, id);
}

void EpollControl(wa_host_call_ctx_t& ctx) {
    auto& dbt = *ctx.dbt;
    const int epfd = ctx.args[0], op = ctx.args[1], fd = ctx.args[2];
    const int host = HostFd(dbt, fd);
    if (HostFd(dbt, epfd) < 0 || host < 0) return Fail(ctx, EBADF);
    const auto epoll = EpollFor(dbt, epfd);
    if (!epoll || op < 1 || op > 3) return Fail(ctx, EINVAL);
    if (EpollFor(dbt, fd)) return Fail(ctx, epfd == fd ? EINVAL : ENOTSUP);
    struct stat metadata{};
    if (fstat(host, &metadata) < 0) return PosixResult(ctx, -1);
    if (S_ISREG(metadata.st_mode) || S_ISDIR(metadata.st_mode)) return Fail(ctx, EPERM);
    const auto found = epoll->interests.find(fd);
    if (op == 1 && found != epoll->interests.end()) return Fail(ctx, EEXIST);
    if (op != 1 && found == epoll->interests.end()) return Fail(ctx, ENOENT);
    if (op == 2) {
        epoll->interests.erase(found);
        return;
    }
    if (!ctx.args[3]) return Fail(ctx, EFAULT);
    const auto event = GuestMemory(ctx).Read<EpollEvent>(ctx.args[3]);
    if (event.events & ~(kEpollPollFlags | kEpollOneShot)) return Fail(ctx, ENOTSUP);
    epoll->interests.insert_or_assign(fd, GuestEpoll::Interest{event.events, event.data, ++epoll->revision});
}

void EpollWait(wa_host_call_ctx_t& ctx) {
    auto& dbt = *ctx.dbt;
    if (HostFd(dbt, ctx.args[0]) < 0) return Fail(ctx, EBADF);
    const auto epoll = EpollFor(dbt, ctx.args[0]);
    const int maximum = ctx.args[2];
    if (!epoll || maximum <= 0) return Fail(ctx, EINVAL);
    if (!ctx.args[1]) return Fail(ctx, EFAULT);
    std::vector<pollfd> descriptors;
    std::vector<std::pair<int, uint64_t>> guest_fds;
    const auto& interests = epoll->interests;
    const auto start = interests.upper_bound(epoll->last_ready);
    for (auto it = start; guest_fds.size() < interests.size();) {
        if (it == interests.end()) it = interests.begin();
        const auto& [fd, interest] = *it++;
        guest_fds.emplace_back(fd, interest.revision);
        const auto host = interest.enabled ? HostFd(dbt, fd) : -1;
        descriptors.push_back({host, static_cast<short>(PollEvents(interest.events, true)), 0});
    }
    const auto result = PollWait(ctx, descriptors, static_cast<int32_t>(ctx.args[3]));
    if (result < 0) return PosixResult(ctx, result);
    const GuestMemory memory(ctx);
    int written = 0;
    for (size_t i = 0; i < descriptors.size() && written < maximum; ++i) {
        const auto [fd, revision] = guest_fds[i];
        const auto found = epoll->interests.find(fd);
        if (found == epoll->interests.end() || found->second.revision != revision) continue;
        auto& interest = found->second;
        const auto ready = uint32_t(PollEvents(descriptors[i].revents, false)) &
                           (interest.events | kEpollAlways);
        if (!ready) continue;
        memory.Write(ctx.args[1] + written * sizeof(EpollEvent), EpollEvent{ready, 0, interest.data});
        ++written;
        epoll->last_ready = fd;
        if (interest.events & kEpollOneShot) interest.enabled = false;
    }
    ctx.ret0 = written;
}

void EpollPwait(wa_host_call_ctx_t& ctx) {
    auto& thread = *ctx.dbt->running;
    struct RestoreMask {
        GuestThread& thread;
        uint64_t previous;
        ~RestoreMask() { thread.signal_mask = previous; }
    } restore{thread, thread.signal_mask};
    if (ctx.args[4]) {
        // SIGKILL and SIGSTOP cannot be blocked.
        constexpr uint64_t unblocked = (uint64_t{1} << 8) | (uint64_t{1} << 18);
        thread.signal_mask = GuestMemory(ctx).ReadWord(ctx.args[4]) & ~unblocked;
    }
    EpollWait(ctx);
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

template<bool Follow> void ChangeOwner(wa_host_call_ctx_t& ctx) {
    const auto path = Path(ctx, ctx.args[0]);
    if (FindMountedFile(*ctx.dbt, path)) return Fail(ctx, EROFS);
    struct stat current{};
    if ((Follow ? stat : lstat)(path.c_str(), &current)) return PosixResult(ctx, -1);
    const auto uid = static_cast<uint32_t>(ctx.args[1]);
    const auto gid = static_cast<uint32_t>(ctx.args[2]);
    // Guest applications have no privilege to change backing-file ownership.
    if (current.st_uid != geteuid() || (uid != UINT32_MAX && uid != current.st_uid) ||
        (gid != UINT32_MAX && gid != current.st_gid)) return Fail(ctx, EPERM);
}

void Futimens(wa_host_call_ctx_t& ctx) {
    const int fd = HostFd(*ctx.dbt, ctx.args[0]);
    const auto source = ctx.args[1];
    const GuestMemory mem(ctx);
    struct timespec times[2]{};
    if (source) {
        for (size_t i = 0; i < 2; ++i) {
            const auto address = source + i * 2 * mem.width();
            times[i].tv_sec = SignedWord(mem, mem.ReadWord(address));
            const auto fraction = SignedWord(mem, mem.ReadWord(address + mem.width()));
            switch (fraction) {
            case 0x3fffffff: times[i].tv_nsec = UTIME_NOW; break;
            case 0x3ffffffe: times[i].tv_nsec = UTIME_OMIT; break;
            default: times[i].tv_nsec = fraction; break;
            }
        }
    }
    PosixResult(ctx, futimens(fd, source ? times : nullptr));
}

void TtyName(wa_host_call_ctx_t& ctx) {
    const int fd = HostFd(*ctx.dbt, ctx.args[0]);
    if (fd < 0) {
        ctx.ret0 = GuestErrno(EBADF);
        return;
    }
    std::array<char, PATH_MAX> buffer;
    const auto size = std::min<uint64_t>(buffer.size(), ctx.args[2]);
    ctx.ret0 = GuestErrno(ttyname_r(fd, buffer.data(), size));
    if (!ctx.ret0) GuestMemory(ctx).Write(ctx.args[1], buffer.data(), std::strlen(buffer.data()) + 1);
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

template <bool Wide> void Sendfile(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    const auto out = HostFd(*ctx.dbt, ctx.args[0]), in = HostFd(*ctx.dbt, ctx.args[1]);
    if (out < 0 || in < 0)
        return PosixResult(ctx, -1);
    const auto offset_address = ctx.args[2];
    const auto limit = std::min<uint64_t>(ctx.args[3], kIoLimit);
    std::vector<char> bytes(limit);
    const auto offset = !offset_address ? 0 : Wide ? mem.Read<int64_t>(offset_address)
                                                  : SignedWord(mem, mem.ReadWord(offset_address));
    const auto count = offset_address ? pread(in, bytes.data(), bytes.size(), offset)
                                      : read(in, bytes.data(), bytes.size());
    if (count <= 0)
        return PosixResult(ctx, count);
    const auto result = write(out, bytes.data(), count);
    PosixResult(ctx, result);
    if (offset_address && result > 0) {
        if constexpr (Wide) mem.Write<int64_t>(offset_address, offset + result);
        else mem.WriteWord(offset_address, offset + result);
    }
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

template <bool Timer> void Eventfd(wa_host_call_ctx_t& ctx) {
    auto& dbt = *ctx.dbt;
    const auto flags = ctx.args[1];
    if (flags & ~(Timer ? UINT64_C(0x80800) : UINT64_C(0x80801)))
        return Fail(ctx, EINVAL);
    auto event = std::make_shared<GuestEvent>();
    if constexpr (Timer) {
        if (ctx.args[0] != 0 && ctx.args[0] != 1 && ctx.args[0] != 7) return Fail(ctx, EINVAL);
        event->timer_clock = ctx.args[0];
    } else {
        event->count = static_cast<uint32_t>(ctx.args[0]);
        event->semaphore = (flags & 1) != 0;
    }
    int pipe_fds[2];
    if (pipe(pipe_fds))
        return PosixResult(ctx, -1);
    event->notification = pipe_fds[1];
    event->id = dbt.bionic->next_event++;
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

std::chrono::nanoseconds TimerNow(const GuestEvent& event) {
    using namespace std::chrono;
    return event.timer_clock == 0 ? duration_cast<nanoseconds>(system_clock::now().time_since_epoch())
                                  : duration_cast<nanoseconds>(Clock::now().time_since_epoch());
}

void RefreshTimer(GuestEvent& event) {
    using namespace std::chrono;
    if (event.timer_clock < 0 || event.timer_due == nanoseconds{}) return;
    const auto now = TimerNow(event);
    if (now < event.timer_due) return;
    const auto overdue = now - event.timer_due;
    const uint64_t elapsed = event.timer_interval.count() ? uint64_t(overdue / event.timer_interval) + 1 : 1;
    if (!event.count) {
        const char token = 1;
        if (write(event.notification, &token, 1) != 1) return;
    }
    event.count += std::min(elapsed, UINT64_MAX - event.count);
    if (event.timer_interval.count()) {
        const auto next = event.timer_interval - overdue % event.timer_interval;
        event.timer_due = now + std::min(next, nanoseconds::max() - now);
    } else {
        event.timer_due = {};
    }
}

std::optional<std::chrono::nanoseconds> ReadTimerDuration(const GuestMemory& memory, uint64_t address) {
    using namespace std::chrono;
    const auto sec = SignedWord(memory, memory.ReadWord(address));
    const auto nanos = SignedWord(memory, memory.ReadWord(address + memory.width()));
    if (sec < 0 || nanos < 0 || nanos >= 1'000'000'000 ||
        sec > (nanoseconds::max().count() - nanos) / 1'000'000'000) return std::nullopt;
    return seconds(sec) + nanoseconds(nanos);
}

void WriteTimerDuration(const GuestMemory& memory, uint64_t address, std::chrono::nanoseconds duration) {
    using namespace std::chrono;
    const auto whole = duration_cast<seconds>(duration);
    memory.WriteWord(address, whole.count());
    memory.WriteWord(address + memory.width(), (duration - whole).count());
}

template <bool Set> void TimerTime(wa_host_call_ctx_t& ctx) {
    using namespace std::chrono;
    auto& dbt = *ctx.dbt;
    const auto event = EventFor(dbt, ctx.args[0]);
    if (!event || event->timer_clock < 0) return Fail(ctx, HostFd(dbt, ctx.args[0]) < 0 ? EBADF : EINVAL);
    const GuestMemory memory(ctx);
    const auto timespec_size = 2 * memory.width();
    const auto write_state = [&](uint64_t output) {
        RefreshTimer(*event);
        WriteTimerDuration(memory, output, event->timer_interval);
        WriteTimerDuration(memory, output + timespec_size,
                           std::max(nanoseconds{}, event->timer_due - TimerNow(*event)));
    };
    if constexpr (Set) {
        const auto flags = ctx.args[1];
        if (flags & ~3u) return Fail(ctx, EINVAL);
        if (flags & 2) return Fail(ctx, ENOTSUP);
        if (!ctx.args[2]) return Fail(ctx, EFAULT);
        const auto interval = ReadTimerDuration(memory, ctx.args[2]);
        const auto value = ReadTimerDuration(memory, ctx.args[2] + timespec_size);
        if (!interval || !value) return Fail(ctx, EINVAL);
        const auto now = TimerNow(*event);
        if (!(flags & 1) && *value > nanoseconds::max() - now) return Fail(ctx, EOVERFLOW);
        if (ctx.args[3]) write_state(ctx.args[3]);
        if (event->count) {
            char marker;
            ::read(HostFd(dbt, ctx.args[0]), &marker, 1);
        }
        event->count = 0;
        event->timer_interval = *interval;
        event->timer_due = !value->count() || flags & 1 ? *value : now + *value;
        RefreshTimer(*event);
    } else {
        if (!ctx.args[1]) return Fail(ctx, EFAULT);
        write_state(ctx.args[1]);
    }
}

void Isatty(wa_host_call_ctx_t& ctx) {
    ctx.ret0 = isatty(HostFd(*ctx.dbt, ctx.args[0]));
    if (!ctx.ret0)
        SetErrno(*ctx.dbt, GuestErrno(errno));
}

void TerminalAttributes(wa_host_call_ctx_t& ctx) {
    if (HostFd(*ctx.dbt, ctx.args[0]) < 0) return PosixResult(ctx, -1);
    // Guest application descriptors have no terminal line discipline.
    Fail(ctx, ENOTTY);
}

enum class NotifyWatch { Create, Add, Remove };
template <NotifyWatch operation> void Inotify(wa_host_call_ctx_t& ctx) {
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
    auto& dbt = *ctx.dbt;
    if constexpr (operation == NotifyWatch::Create) {
        if (ctx.args[0] & ~0x80800u) return Fail(ctx, EINVAL);
        const auto flags = (ctx.args[0] & 0x80000 ? IN_CLOEXEC : 0) |
                           (ctx.args[0] & 0x800 ? IN_NONBLOCK : 0);
        return PosixResult(ctx, AdoptFd(dbt, inotify_init1(flags)));
    } else if constexpr (operation == NotifyWatch::Add) {
        const auto fd = HostFd(dbt, ctx.args[0]);
        return PosixResult(ctx, inotify_add_watch(fd, Path(ctx, ctx.args[1]).c_str(), ctx.args[2]));
    } else {
        const auto fd = HostFd(dbt, ctx.args[0]);
        return PosixResult(ctx, inotify_rm_watch(fd, ctx.args[1]));
    }
#else
    Fail(ctx, ENOSYS);
#endif
}

} // namespace

void RefreshTimers(wa_dbt& dbt) {
    for (const auto& [fd, entry] : dbt.bionic->descriptors)
        if (entry.event) RefreshTimer(*entry.event);
}

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
        {"dup3", Dispatch<DuplicateFd<true, true>>},
        FD(fsync),
        FD(flock),
        {"isatty", Dispatch<Isatty>},
        {"tcgetattr", Dispatch<TerminalAttributes>},
        {"tcsetattr", Dispatch<TerminalAttributes>},
        {"read", Dispatch<Transfer<Io::Read>>},
        {"__read_chk", Dispatch<Transfer<Io::Read, true>>},
        {"write", Dispatch<Transfer<Io::Write>>},
        {"__write_chk", Dispatch<Transfer<Io::Write, true>>},
        {"pread", Dispatch<Transfer<Io::Pread>>},
        {"pread64", Dispatch<Transfer<Io::Pread64>>},
        {"__pread_chk", Dispatch<Transfer<Io::Pread, true>>},
        {"__pread64_chk", Dispatch<Transfer<Io::Pread64, true>>},
        {"pwrite", Dispatch<Transfer<Io::Pwrite>>},
        {"pwrite64", Dispatch<Transfer<Io::Pwrite64>>},
        {"__pwrite_chk", Dispatch<Transfer<Io::Pwrite, true>>},
        {"__pwrite64_chk", Dispatch<Transfer<Io::Pwrite64, true>>},
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
        PATH(faccessat, AccessAt),
        PATH(unlink, Unlink),
        PATH(mkdir, Mkdir),
        PATH(mkdirat, MkdirAt),
        PATH(mkfifo, Mkfifo),
        PATH(mknod, Mknod),
        PATH(rmdir, Rmdir),
        PATH(rename, Rename),
        PATH(renameat, RenameAt),
        PATH(remove, Remove),
        PATH(link, Link),
        PATH(symlink, Symlink),
        PATH(symlinkat, SymlinkAt),
        PATH(chmod, Chmod),
        PATH(fchmodat, ChmodAt),
        PATH(unlinkat, UnlinkAt),
        PATH(truncate, Truncate),
        PATH(truncate64, Truncate64),
        PATH(readlink, Readlink),
        {"__readlink_chk", Dispatch<PathCall<FilePath::Readlink, true>>},
        PATH(readlinkat, ReadlinkAt),
        PATH(realpath, Realpath),
        PATH(chdir, Chdir),
        PATH(pathconf, Pathconf),
        PATH(utime, Utime),
        PATH(utimes, Utimes),
        PATH(utimensat, UtimensAt),
        {"getcwd", Dispatch<Getcwd>},
        {"mkstemp", Dispatch<Mkstemp<false>>},
        {"mkstemp64", Dispatch<Mkstemp<false>>},
        {"mkstemps", Dispatch<Mkstemp<true>>},
        {"mkdtemp", Dispatch<Mkdtemp>},
        {"nftw", Dispatch<WalkFiles>},
        {"fnmatch", Dispatch<FilenameMatch>},
#if !defined(__EMSCRIPTEN__)
        {"fts_open", Dispatch<FileTree<Walk::Open>>},
        {"fts_read", Dispatch<FileTree<Walk::Read>>},
        {"fts_set", Dispatch<FileTree<Walk::Set>>},
        {"fts_close", Dispatch<FileTree<Walk::Close>>},
#endif
        {"tempnam", Dispatch<Tempnam<false>>},
        {"tmpnam", Dispatch<Tempnam<true>>},
        DIR(opendir, Open),
        DIR(fdopendir, FdOpen),
        DIR(readdir, Read),
        {"scandir", Dispatch<ScanDirectory>},
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
        {"epoll_create", Dispatch<EpollCreate<false>>},
        {"epoll_create1", Dispatch<EpollCreate<true>>},
        {"epoll_ctl", Dispatch<EpollControl>},
        {"epoll_wait", Dispatch<EpollWait>},
        {"epoll_pwait", Dispatch<EpollPwait>},
        {"select", Dispatch<Select>},
        {"__FD_ISSET_chk", Dispatch<FdSet<0>>},
        {"__FD_SET_chk", Dispatch<FdSet<1>>},
        {"__FD_CLR_chk", Dispatch<FdSet<2>>},
        {"fchmod", Dispatch<Fchmod>},
        {"fchown", Dispatch<Fchown>},
        {"chown", Dispatch<ChangeOwner<true>>},
        {"lchown", Dispatch<ChangeOwner<false>>},
        {"futimens", Dispatch<Futimens>},
        {"ttyname_r", Dispatch<TtyName>},
        {"fdatasync", Dispatch<Fdatasync>},
        {"posix_fallocate", Dispatch<PosixAllocation<true>>},
        {"posix_fadvise", Dispatch<PosixAllocation<false>>},
        {"posix_fadvise64", Dispatch<PosixAllocation<false, true>>},
        {"posix_fallocate64", Dispatch<PosixAllocation<true, true>>},
        {"sendfile", Dispatch<Sendfile<false>>},
        {"sendfile64", Dispatch<Sendfile<true>>},
        {"setmntent", Dispatch<MountTable>},
        {"getmntent_r", Dispatch<ReadMount>},
        {"endmntent", Dispatch<CloseMount>},
        {"eventfd", Dispatch<Eventfd<false>>},
        {"timerfd_create", Dispatch<Eventfd<true>>},
        {"timerfd_gettime", Dispatch<TimerTime<false>>},
        {"timerfd_settime", Dispatch<TimerTime<true>>},
        {"inotify_init1", Dispatch<Inotify<NotifyWatch::Create>>},
        {"inotify_add_watch", Dispatch<Inotify<NotifyWatch::Add>>},
        {"inotify_rm_watch", Dispatch<Inotify<NotifyWatch::Remove>>},
    };
#undef FD
#undef DIR
#undef PATH
    Register(dbt, "libc.so", c);
    RegisterMappings(dbt);
}

} // namespace wa::dbt
