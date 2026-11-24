#include "bionic_internal.h"
#include "bionic_scan.h"
#include "guest_format.h"

#include <cerrno>
#include <cwchar>
#include <unistd.h>

namespace wa::dbt {
namespace {

constexpr size_t kFormattedLimit = 1 << 20;

size_t FileSize(GuestMemory mem) {
    return mem.width() == 8 ? 152 : 84;
}

void WriteFileHeader(GuestMemory mem, uint64_t address, int fd, unsigned flags) {
    if (mem.width() == 8) {
        mem.Write<uint32_t>(address + 16, flags);
        mem.Write<int32_t>(address + 20, fd);
    } else {
        mem.Write<uint16_t>(address + 12, flags);
        mem.Write<int16_t>(address + 14, fd);
    }
}

void Refresh(GuestMemory mem, uint64_t address, FILE* file) {
    const auto offset = address + mem.width() + 8;
    const auto flags = mem.width() == 8 ? mem.Read<uint32_t>(offset) : mem.Read<uint16_t>(offset);
    const auto bits = (std::feof(file) ? 0x20 : 0) | (std::ferror(file) ? 0x40 : 0);
    if (mem.width() == 8)
        mem.Write<uint32_t>(offset, (flags & ~0x60u) | bits);
    else
        mem.Write<uint16_t>(offset, (flags & ~0x60u) | bits);
    // Historical getc macros decrement _r before calling __srget.
    mem.Write<int32_t>(address + mem.width(), 0);
}

GuestFile* Stream(wa_host_call_ctx_t& ctx, uint64_t address) {
    auto& dbt = *ctx.dbt;
    auto& files = dbt.bionic->files;
    while (true) {
        const auto it = files.find(address);
        if (it == files.end()) {
            Fail(ctx, EBADF);
            return nullptr;
        }
        auto& stream = it->second;
        if (!stream.owner || stream.owner == dbt.running->id) {
            dbt.running->wait = {};
            return &stream;
        }
        if (const auto error = Park(dbt, WaitKind::File, address)) {
            SetErrno(dbt, error);
            ctx.ret0 = UINT64_MAX;
            return nullptr;
        }
    }
}

unsigned ModeFlags(std::string_view mode) {
    if (mode.find('+') != mode.npos)
        return 0x10;
    return mode.starts_with('r') ? 4 : 8;
}

void InstallStream(wa_host_call_ctx_t& ctx, FILE* file, unsigned flags, int descriptor = -1) {
    if (!file) {
        SetErrno(*ctx.dbt, GuestErrno(errno));
        return;
    }
    auto& dbt = *ctx.dbt;
    const GuestMemory mem(ctx);
    const auto address = AllocateGuest(dbt, FileSize(mem));
    if (!address) {
        std::fclose(file);
        SetErrno(dbt, GuestErrno(ENOMEM));
        return;
    }
    const bool bind_descriptor = descriptor < 0;
    try {
        if (bind_descriptor)
            descriptor = AdoptFd(dbt, fileno(file), false);
        if (descriptor < 0) {
            const auto error = errno;
            std::fclose(file);
            FreeGuest(dbt, address);
            SetErrno(dbt, GuestErrno(error));
            return;
        }
        WriteFileHeader(mem, address, descriptor, flags);
        dbt.bionic->files.emplace(address, GuestFile{file, true, descriptor});
    } catch (...) {
        std::fclose(file);
        FreeGuest(dbt, address);
        if (bind_descriptor && descriptor >= 0)
            dbt.bionic->descriptors.erase(descriptor);
        throw;
    }
    ctx.ret0 = address;
}

enum class Open { Path, Descriptor, Temporary, Reopen };
template <Open op> void OpenFile(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    if constexpr (op == Open::Temporary) {
        InstallStream(ctx, std::tmpfile(), 0x10);
    } else {
        const auto mode = mem.String(ctx.args[1], 32);
        if constexpr (op == Open::Descriptor) {
            const auto descriptor = static_cast<int32_t>(ctx.args[0]);
            const auto fd = HostFd(*ctx.dbt, descriptor);
            if (fd < 0 || EventFor(*ctx.dbt, descriptor)) {
                SetErrno(*ctx.dbt, GuestErrno(fd < 0 ? EBADF : EINVAL));
                return;
            }
            const auto copy = dup(fd);
            auto* file = copy >= 0 ? fdopen(copy, mode.c_str()) : nullptr;
            if (!file && copy >= 0) {
                const auto error = errno;
                close(copy);
                errno = error;
            }
            InstallStream(ctx, file, ModeFlags(mode), descriptor);
        } else {
            const auto path = GuestPath(ctx, ctx.args[0]);
            if constexpr (op == Open::Path) {
                InstallStream(ctx, std::fopen(path.c_str(), mode.c_str()), ModeFlags(mode));
            } else {
                auto* file = Stream(ctx, ctx.args[2]);
                if (!file) {
                    ctx.ret0 = 0;
                    return;
                }
                if (!file->owned) {
                    // Redirect this guest's standard stream independently of the host.
                    const auto result = std::fopen(path.c_str(), mode.c_str());
                    if (result) {
                        file->host = result;
                        file->owned = true;
                    } else
                        SetErrno(*ctx.dbt, GuestErrno(errno));
                    if (!result)
                        return;
                } else {
                    const auto result = std::freopen(path.c_str(), mode.c_str(), file->host);
                    if (!result) {
                        SetErrno(*ctx.dbt, GuestErrno(errno));
                        CloseFd(*ctx.dbt, file->descriptor);
                        ctx.dbt->bionic->files.erase(ctx.args[2]);
                        Wake(*ctx.dbt, WaitKind::File, ctx.args[2]);
                        return;
                    }
                    file->host = result;
                }
                CloseFd(*ctx.dbt, file->descriptor);
                ctx.dbt->bionic->descriptors[file->descriptor] = {fileno(file->host), false, {}};
                WriteFileHeader(mem, ctx.args[2], file->descriptor, ModeFlags(mode));
                ctx.ret0 = ctx.args[2];
            }
        }
    }
}

void CloseFile(wa_host_call_ctx_t& ctx) {
    auto* stream = Stream(ctx, ctx.args[0]);
    if (!stream)
        return;
    auto& dbt = *ctx.dbt;
    const GuestMemory mem(ctx);
    const auto address = ctx.args[0];
    PosixResult(ctx, stream->owned ? std::fclose(stream->host) : std::fflush(stream->host));
    if (CloseFd(dbt, stream->descriptor) && ctx.ret0 == 0)
        PosixResult(ctx, -1);
    dbt.bionic->files.erase(address);
    Wake(dbt, WaitKind::File, address);
    if (address < dbt.bionic->stdio || address >= dbt.bionic->stdio + 3 * FileSize(mem))
        FreeGuest(dbt, address);
    else
        WriteFileHeader(mem, address, -1, 0);
}

template <bool Read> void Transfer(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    const auto address = ctx.args[0], width = ctx.args[1], count = ctx.args[2],
               stream = ctx.args[3];
    auto* file = Stream(ctx, stream);
    if (!file) {
        ctx.ret0 = 0;
        return;
    }
    if (!count || !width)
        return;
    if (width > UINT32_MAX || count > UINT32_MAX / width || address > UINT32_MAX ||
        width * count > UINT32_MAX - address) {
        SetErrno(*ctx.dbt, GuestErrno(EOVERFLOW));
        return;
    }
    const auto total = width * count;
    std::array<uint8_t, WA_PAGE_SIZE> buffer;
    uint64_t transferred = 0;
    while (transferred < total) {
        const auto size = std::min<uint64_t>(buffer.size(), total - transferred);
        size_t actual;
        if constexpr (Read) {
            actual = std::fread(buffer.data(), 1, size, file->host);
            mem.Write(address + transferred, buffer.data(), actual);
        } else {
            mem.Read(address + transferred, buffer.data(), size);
            actual = std::fwrite(buffer.data(), 1, size, file->host);
        }
        transferred += actual;
        if (actual < size)
            break;
    }
    if (transferred < total && std::ferror(file->host))
        SetErrno(*ctx.dbt, GuestErrno(errno));
    Refresh(mem, stream, file->host);
    ctx.ret0 = transferred / width;
}

enum class Character { Get, Put, Unget, GetWide, PutWide, UngetWide, GetDefault, PutDefault };
template <Character op> void FileChar(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    uint64_t address;
    if constexpr (op == Character::GetDefault || op == Character::PutDefault) {
        address = ctx.dbt->bionic->stdio + (op == Character::PutDefault ? FileSize(mem) : 0);
    } else if constexpr (op == Character::Get || op == Character::GetWide) {
        address = ctx.args[0];
    } else {
        address = ctx.args[1];
    }
    auto* file = Stream(ctx, address);
    if (!file)
        return;
    int64_t result;
    if constexpr (op == Character::Get || op == Character::GetDefault)
        result = std::fgetc(file->host);
    if constexpr (op == Character::Put || op == Character::PutDefault)
        result = std::fputc(ctx.args[0], file->host);
    if constexpr (op == Character::Unget)
        result = std::ungetc(ctx.args[0], file->host);
    if constexpr (op == Character::GetWide)
        result = static_cast<int32_t>(std::fgetwc(file->host));
    if constexpr (op == Character::PutWide)
        result = static_cast<int32_t>(std::fputwc(ctx.args[0], file->host));
    if constexpr (op == Character::UngetWide)
        result = static_cast<int32_t>(std::ungetwc(ctx.args[0], file->host));
    ctx.ret0 = result;
    if (result == EOF && std::ferror(file->host))
        SetErrno(*ctx.dbt, GuestErrno(errno));
    Refresh(mem, address, file->host);
}

enum class Line { Get, Put, Puts, GetLine };
template <Line op> void FileLine(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    const auto address = op == Line::Puts
                             ? ctx.dbt->bionic->stdio + FileSize(mem)
                             : ctx.args[op == Line::Get || op == Line::GetLine ? 2 : 1];
    auto* file = Stream(ctx, address);
    if (!file) {
        if constexpr (op == Line::Get)
            ctx.ret0 = 0;
        return;
    }
    if constexpr (op == Line::Get) {
        const auto capacity = static_cast<int32_t>(ctx.args[1]);
        if (capacity <= 0)
            return;
        std::vector<char> text(std::min<size_t>(capacity, kFormattedLimit));
        size_t size = 0;
        while (size + 1 < text.size()) {
            const auto value = std::fgetc(file->host);
            if (value == EOF)
                break;
            text[size++] = char(value);
            if (value == '\n')
                break;
        }
        if (size || capacity == 1) {
            text[size] = 0;
            mem.Write(ctx.args[0], text.data(), size + 1);
            ctx.ret0 = ctx.args[0];
        }
    } else if constexpr (op == Line::GetLine) {
        char* line = nullptr;
        size_t capacity = 0;
        const auto count = getline(&line, &capacity, file->host);
        std::unique_ptr<char, decltype(&std::free)> buffer(line, std::free);
        if (count >= 0) {
            auto dst = mem.ReadWord(ctx.args[0]);
            if (!dst || mem.ReadWord(ctx.args[1]) <= uint64_t(count)) {
                const auto replacement = AllocateGuest(*ctx.dbt, count + 1);
                if (!replacement)
                    return Fail(ctx, ENOMEM);
                FreeGuest(*ctx.dbt, dst);
                dst = replacement;
                mem.WriteWord(ctx.args[0], dst);
                mem.WriteWord(ctx.args[1], count + 1);
            }
            mem.Write(dst, line, count + 1);
        }
        ctx.ret0 = count;
    } else {
        const auto text = mem.String(ctx.args[0]);
        ctx.ret0 = std::fputs(text.c_str(), file->host);
        if constexpr (op == Line::Puts) {
            if (static_cast<int32_t>(ctx.ret0) >= 0)
                ctx.ret0 = std::fputc('\n', file->host);
        }
    }
    if (std::ferror(file->host))
        SetErrno(*ctx.dbt, GuestErrno(errno));
    Refresh(mem, address, file->host);
}

enum class Position { Seek, Seek64, Tell, Tell64, Rewind };
template <Position op> void FilePosition(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    CallReader args(ctx);
    const auto address = args.Next();
    auto* file = Stream(ctx, address);
    if (!file)
        return;
    if constexpr (op == Position::Tell || op == Position::Tell64) {
        const auto offset = ftello(file->host);
        if (offset < 0)
            PosixResult(ctx, offset);
        else if (mem.width() == 4 && op == Position::Tell && offset > INT32_MAX)
            return Fail(ctx, EOVERFLOW);
        SetScalarResult(ctx, ArgKind::Long, ctx.ret0 ? ctx.ret0 : offset);
    } else if constexpr (op == Position::Rewind) {
        std::rewind(file->host);
    } else {
        const auto offset = op == Position::Seek64 ? static_cast<int64_t>(args.Next(ArgKind::Long))
                                                   : SignedWord(mem, args.Next());
        PosixResult(ctx, fseeko(file->host, offset, args.Next()));
    }
    Refresh(mem, address, file->host);
}

enum class StreamOp { Flush, Eof, Error, Clear, Descriptor, Buffer, Buf, Lock, Unlock, TryLock };
template <StreamOp op> void Control(wa_host_call_ctx_t& ctx) {
    auto& dbt = *ctx.dbt;
    const GuestMemory mem(ctx);
    const auto address = ctx.args[0];
    if constexpr (op == StreamOp::Flush) {
        if (!address) {
            for (const auto& [handle, stream] : dbt.bionic->files)
                if (std::fflush(stream.host) == EOF)
                    PosixResult(ctx, EOF);
            return;
        }
    }
    if constexpr (op == StreamOp::TryLock) {
        const auto found = dbt.bionic->files.find(address);
        if (found != dbt.bionic->files.end() && found->second.owner &&
            found->second.owner != dbt.running->id) {
            ctx.ret0 = 1;
            return;
        }
    }
    auto* file = Stream(ctx, address);
    if (!file)
        return;
    if constexpr (op == StreamOp::Eof)
        ctx.ret0 = std::feof(file->host);
    if constexpr (op == StreamOp::Error)
        ctx.ret0 = std::ferror(file->host);
    if constexpr (op == StreamOp::Clear)
        std::clearerr(file->host);
    if constexpr (op == StreamOp::Descriptor)
        ctx.ret0 = file->descriptor;
    if constexpr (op == StreamOp::Flush)
        PosixResult(ctx, std::fflush(file->host));
    if constexpr (op == StreamOp::Buf || op == StreamOp::Buffer) {
        const auto mode = op == StreamOp::Buf ? (ctx.args[1] ? 0 : 2) : ctx.args[2];
        const auto size = op == StreamOp::Buf ? BUFSIZ : ctx.args[3];
        const int modes[] = {_IOFBF, _IOLBF, _IONBF};
        if (mode > 2)
            return Fail(ctx, EINVAL);
        PosixResult(ctx, std::setvbuf(file->host, nullptr, modes[mode], size));
    }
    if constexpr (op == StreamOp::Lock || op == StreamOp::TryLock) {
        file->owner = dbt.running->id;
        ++file->lock_count;
    }
    if constexpr (op == StreamOp::Unlock) {
        if (file->owner == dbt.running->id && !--file->lock_count) {
            file->owner = 0;
            Wake(dbt, WaitKind::File, address);
        }
    }
    Refresh(mem, address, file->host);
}

enum class Format { Stdout, File, String, Bounded, Allocated, Checked, BoundedChecked };
template <Format op, bool Va> void Print(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    constexpr bool bounded = op == Format::Bounded || op == Format::BoundedChecked;
    constexpr auto fixed = op == Format::Stdout           ? 1
                           : op == Format::Bounded        ? 3
                           : op == Format::Checked        ? 4
                           : op == Format::BoundedChecked ? 5
                                                          : 2;
    CallReader reader(ctx);
    std::array<uint64_t, fixed> named{};
    for (auto& word : named)
        word = reader.Next();
    const auto va = Va ? std::optional(reader.Next()) : std::nullopt;
    const auto cap = bounded ? named[1] : kFormattedLimit + 1;
    const auto object_size = op == Format::Checked          ? named[2]
                             : op == Format::BoundedChecked ? named[3]
                                                            : UINT64_MAX;
    if (bounded && cap > object_size)
        throw GuestFault{WA_ERR_FAULT};
    const auto address = op == Format::Stdout ? ctx.dbt->bionic->stdio + FileSize(mem) : named[0];
    GuestFile* stream = nullptr;
    if constexpr (op == Format::Stdout || op == Format::File) {
        stream = Stream(ctx, address);
        if (!stream)
            return;
    }
    size_t length = 0;
    const auto result = FormatGuest(ctx, named.back(), fixed, va, cap ? cap - 1 : 0, &length);
    if (length > INT32_MAX)
        return Fail(ctx, EOVERFLOW);
    if constexpr (op == Format::Stdout || op == Format::File) {
        if (length != result.size())
            return Fail(ctx, EOVERFLOW);
        const auto size = std::fwrite(result.data(), 1, result.size(), stream->host);
        Refresh(mem, address, stream->host);
        if (size != result.size())
            return Fail(ctx, errno);
    } else if constexpr (op == Format::Allocated) {
        if (length != result.size())
            return Fail(ctx, EOVERFLOW);
        mem.WriteWord(named[0], CopyString(*ctx.dbt, result));
    } else {
        if constexpr (!bounded) {
            if (length >= object_size)
                throw GuestFault{WA_ERR_FAULT};
            if (length != result.size())
                return Fail(ctx, EOVERFLOW);
        }
        if (cap)
            mem.Write(named[0], result.c_str(), result.size() + 1);
    }
    ctx.ret0 = length;
}

template <bool File, bool Va> void Scan(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    if constexpr (File) {
        auto* file = Stream(ctx, ctx.args[0]);
        if (!file)
            return;
        ctx.ret0 = ScanGuest(ctx, {}, file->host, 2, ctx.args[1],
                             Va ? std::optional(ctx.args[2]) : std::nullopt);
        Refresh(mem, ctx.args[0], file->host);
    } else {
        ctx.ret0 = ScanGuest(ctx, mem.String(ctx.args[0]), nullptr, 2, ctx.args[1],
                             Va ? std::optional(ctx.args[2]) : std::nullopt);
    }
}

void Swprintf(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    size_t bytes = 0;
    const auto format = WideGuestString(mem, ctx.args[2], kFormattedLimit);
    const auto output = FormatGuestText(ctx, format, 3, {}, kFormattedLimit, &bytes);
    if (bytes != output.size())
        return Fail(ctx, EOVERFLOW);
    auto wide = Utf8ToWide(output);
    if (wide.size() >= ctx.args[1])
        return Fail(ctx, EOVERFLOW);
    ctx.ret0 = wide.size();
    wide.push_back(0);
    mem.Write(ctx.args[0], wide.data(), wide.size() * sizeof(uint32_t));
}

void Perror(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    const auto error = mem.Read<int32_t>(ErrnoAddress(*ctx.dbt));
    const auto prefix = ctx.args[0] ? mem.String(ctx.args[0]) : std::string{};
    auto* file = Stream(ctx, ctx.dbt->bionic->stdio + FileSize(mem) * 2);
    if (!file)
        return;
    std::fprintf(file->host, "%s%s%s\n", prefix.c_str(), prefix.empty() ? "" : ": ",
                 std::strerror(HostErrno(error)));
}

void NoPipeProcess(wa_host_call_ctx_t& ctx) {
    SetErrno(*ctx.dbt, GuestErrno(ENOSYS));
}

} // namespace

void RegisterStdio(wa_dbt& dbt) {
    const GuestMemory mem(dbt.mmu, dbt.arch);
    const auto stride = FileSize(mem);
    const auto data = AllocateGuest(dbt, 3 * (stride + mem.width()));
    if (!data)
        throw GuestFault{WA_ERR_NOMEM};
    dbt.bionic->stdio = data;
    CheckGuest(RegisterData(dbt, "libc.so", "__sF", data));
    const char* names[] = {"stdin", "stdout", "stderr"};
    FILE* standard[] = {stdin, stdout, stderr};
    for (size_t i = 0; i < 3; ++i) {
        const auto file = data + stride * i;
        const auto slot = data + stride * 3 + mem.width() * i;
        WriteFileHeader(mem, file, i, i == 0 ? 4 : i == 1 ? 8 : 10);
        dbt.bionic->files.emplace(file, GuestFile{standard[i], false, static_cast<int>(i)});
        mem.WriteWord(slot, file);
        CheckGuest(RegisterData(dbt, "libc.so", names[i], slot));
    }

    const Builtin c[] = {
        {"fopen", Dispatch<OpenFile<Open::Path>>},
        {"fopen64", Dispatch<OpenFile<Open::Path>>},
        {"fdopen", Dispatch<OpenFile<Open::Descriptor>>},
        {"freopen", Dispatch<OpenFile<Open::Reopen>>},
        {"tmpfile", Dispatch<OpenFile<Open::Temporary>>},
        {"fclose", Dispatch<CloseFile>},
        {"fread", Dispatch<Transfer<true>>},
        {"fwrite", Dispatch<Transfer<false>>},
        {"fgetc", Dispatch<FileChar<Character::Get>>},
        {"getc", Dispatch<FileChar<Character::Get>>},
        {"getc_unlocked", Dispatch<FileChar<Character::Get>>},
        {"__srget", Dispatch<FileChar<Character::Get>>},
        {"getchar", Dispatch<FileChar<Character::GetDefault>>},
        {"fputc", Dispatch<FileChar<Character::Put>>},
        {"putc", Dispatch<FileChar<Character::Put>>},
        {"putc_unlocked", Dispatch<FileChar<Character::Put>>},
        {"__swbuf", Dispatch<FileChar<Character::Put>>},
        {"putchar", Dispatch<FileChar<Character::PutDefault>>},
        {"ungetc", Dispatch<FileChar<Character::Unget>>},
        {"getwc", Dispatch<FileChar<Character::GetWide>>},
        {"fgetwc", Dispatch<FileChar<Character::GetWide>>},
        {"fputwc", Dispatch<FileChar<Character::PutWide>>},
        {"ungetwc", Dispatch<FileChar<Character::UngetWide>>},
        {"puts", Dispatch<FileLine<Line::Puts>>},
        {"fputs", Dispatch<FileLine<Line::Put>>},
        {"fgets", Dispatch<FileLine<Line::Get>>},
        {"getline", Dispatch<FileLine<Line::GetLine>>},
        {"fseek", Dispatch<FilePosition<Position::Seek>>},
        {"fseeko", Dispatch<FilePosition<Position::Seek>>},
        {"fseeko64", Dispatch<FilePosition<Position::Seek64>>},
        {"ftell", Dispatch<FilePosition<Position::Tell>>},
        {"ftello", Dispatch<FilePosition<Position::Tell>>},
        {"ftello64", Dispatch<FilePosition<Position::Tell64>>},
        {"rewind", Dispatch<FilePosition<Position::Rewind>>},
        {"fflush", Dispatch<Control<StreamOp::Flush>>},
        {"feof", Dispatch<Control<StreamOp::Eof>>},
        {"ferror", Dispatch<Control<StreamOp::Error>>},
        {"clearerr", Dispatch<Control<StreamOp::Clear>>},
        {"fileno", Dispatch<Control<StreamOp::Descriptor>>},
        {"setvbuf", Dispatch<Control<StreamOp::Buffer>>},
        {"setbuf", Dispatch<Control<StreamOp::Buf>>},
        {"flockfile", Dispatch<Control<StreamOp::Lock>>},
        {"funlockfile", Dispatch<Control<StreamOp::Unlock>>},
        {"ftrylockfile", Dispatch<Control<StreamOp::TryLock>>},
        {"fprintf", Dispatch<Print<Format::File, false>>},
        {"vfprintf", Dispatch<Print<Format::File, true>>},
        {"printf", Dispatch<Print<Format::Stdout, false>>},
        {"vprintf", Dispatch<Print<Format::Stdout, true>>},
        {"sprintf", Dispatch<Print<Format::String, false>>},
        {"vsprintf", Dispatch<Print<Format::String, true>>},
        {"snprintf", Dispatch<Print<Format::Bounded, false>>},
        {"vsnprintf", Dispatch<Print<Format::Bounded, true>>},
        {"asprintf", Dispatch<Print<Format::Allocated, false>>},
        {"vasprintf", Dispatch<Print<Format::Allocated, true>>},
        {"__sprintf_chk", Dispatch<Print<Format::Checked, false>>},
        {"__vsprintf_chk", Dispatch<Print<Format::Checked, true>>},
        {"__snprintf_chk", Dispatch<Print<Format::BoundedChecked, false>>},
        {"__vsnprintf_chk", Dispatch<Print<Format::BoundedChecked, true>>},
        {"sscanf", Dispatch<Scan<false, false>>},
        {"vsscanf", Dispatch<Scan<false, true>>},
        {"fscanf", Dispatch<Scan<true, false>>},
        {"vfscanf", Dispatch<Scan<true, true>>},
        {"swprintf", Dispatch<Swprintf>},
        {"perror", Dispatch<Perror>},
        {"popen", Dispatch<NoPipeProcess>},
        {"pclose", Dispatch<CloseFile>},
    };
    Register(dbt, "libc.so", c);
}

} // namespace wa::dbt
