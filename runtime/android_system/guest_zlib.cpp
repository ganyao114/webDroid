#include "guest_zlib.h"

#include "bionic_internal.h"
#include <fcntl.h>
#include <unistd.h>

namespace wa::system {
namespace {

using dbt::CallReader;
using dbt::GuestMemory;

// Android uses an ILP32 or LP64 z_stream. Never expose the host z_stream or native pointers.
enum Slot { NextIn, AvailableIn, TotalIn, NextOut, AvailableOut, TotalOut, Message, State,
            Allocate, Release, Opaque, DataType, Adler, Reserved, Count };
constexpr uint32_t kMaxBuffer = 64 * 1024 * 1024;

void Publish(const wa_host_call_ctx_t& ctx, const z_stream& data) {
    const GuestMemory mem(ctx);
    const auto base = ctx.args[0];
    const auto word = mem.width();
    mem.WriteWord(base + TotalIn * word, data.total_in);
    mem.WriteWord(base + TotalOut * word, data.total_out);
    mem.WriteWord(base + Message * word, data.msg ? dbt::InternString(*ctx.dbt, data.msg) : 0);
    mem.WriteWord(base + State * word, data.state ? base : 0);
    mem.Write<int32_t>(base + DataType * word, data.data_type);
    mem.WriteWord(base + Adler * word, data.adler);
}

int TransformBuffer(wa_host_call_ctx_t& ctx, decltype(&compress) transform) {
    const GuestMemory memory(ctx);
    const auto output_size = memory.ReadWord(ctx.args[1]);
    const auto input_size = ctx.args[3];
    if (input_size > kMaxBuffer || output_size > kMaxBuffer) return Z_MEM_ERROR;
    std::vector<uint8_t> input(input_size), output(std::max<uint64_t>(output_size, 1));
    memory.Read(ctx.args[2], input.data(), input.size());
    uLongf produced = output_size;
    const auto result = transform(output.data(), &produced, input.data(), input.size());
    if (produced) memory.Write(ctx.args[0], output.data(), produced);
    memory.WriteWord(ctx.args[1], produced);
    return result;
}

uint64_t CompileFlags(const wa_host_call_ctx_t& ctx) {
    const auto word_code = ctx.arch == WA_ARCH_ARM64 ? 2u : 1u;
    // uInt is 32-bit on both ABIs. uLong, pointers and z_off_t use the guest word width.
    return (zlibCompileFlags() & ~0xffUL) | 1u | (word_code << 2) | (word_code << 4) | (word_code << 6);
}

} // namespace

GuestZlib::Stream::~Stream() {
    if (data.state) {
        if (encode) deflateEnd(&data);
        else inflateEnd(&data);
    }
}

template<bool encode> int GuestZlib::Initialize(wa_host_call_ctx_t& ctx, bool default_window) {
    const GuestMemory mem(ctx);
    CallReader args(ctx);
    const auto base = args.Next();
    int level = 0, method = 0, memory_level = 0, strategy = 0;
    if constexpr (encode) {
        level = static_cast<int32_t>(args.Next());
        if (!default_window) method = static_cast<int32_t>(args.Next());
    }
    const auto bits = default_window ? MAX_WBITS : static_cast<int32_t>(args.Next());
    if constexpr (encode) {
        if (!default_window) {
            memory_level = static_cast<int32_t>(args.Next());
            strategy = static_cast<int32_t>(args.Next());
        }
    }
    const auto version = args.Next();
    if (!version || mem.Read<uint8_t>(version) != ZLIB_VERSION[0] || args.Next() != Count * mem.width())
        return Z_VERSION_ERROR;
    if (!base || mem.ReadWord(base + Allocate * mem.width()) || mem.ReadWord(base + Release * mem.width()))
        return Z_STREAM_ERROR;
    const auto [it, fresh] = streams_.try_emplace(base, encode);
    if (!fresh) return Z_STREAM_ERROR;
    try {
        auto& data = it->second.data;
        int result;
        if constexpr (encode) {
            result = default_window ? deflateInit(&data, level)
                                    : deflateInit2(&data, level, method, bits, memory_level, strategy);
        } else {
            result = inflateInit2(&data, bits);
        }
        if (result == Z_OK) Publish(ctx, data);
        else streams_.erase(it);
        return result;
    } catch (...) {
        streams_.erase(it);
        throw;
    }
}

int GuestZlib::Process(wa_host_call_ctx_t& ctx, bool encode) {
    const GuestMemory mem(ctx);
    const auto base = ctx.args[0];
    const auto word = mem.width();
    const auto it = streams_.find(base);
    if (it == streams_.end() || it->second.encode != encode) return Z_STREAM_ERROR;
    const auto in = mem.ReadWord(base + NextIn * word), out = mem.ReadWord(base + NextOut * word);
    const auto available_in = mem.Read<uint32_t>(base + AvailableIn * word);
    const auto available_out = mem.Read<uint32_t>(base + AvailableOut * word);
    if ((!in && available_in) || !out) return Z_STREAM_ERROR;
    if (available_in > kMaxBuffer || available_out > kMaxBuffer) return Z_MEM_ERROR;
    std::vector<uint8_t> input(available_in), output(std::max(1u, available_out));
    mem.Read(in, input.data(), input.size());
    auto& data = it->second.data;
    data.next_in = input.data();
    data.avail_in = available_in;
    data.next_out = output.data();
    data.avail_out = available_out;
    const auto flush = static_cast<int32_t>(ctx.args[1]);
    const int result = encode ? deflate(&data, flush) : inflate(&data, flush);
    const auto used = available_in - data.avail_in, produced = available_out - data.avail_out;
    data.next_in = nullptr;
    data.next_out = nullptr;
    mem.Write(out, output.data(), produced);
    mem.WriteWord(base + NextIn * word, in + used);
    mem.WriteWord(base + NextOut * word, out + produced);
    mem.Write<uint32_t>(base + AvailableIn * word, data.avail_in);
    mem.Write<uint32_t>(base + AvailableOut * word, data.avail_out);
    Publish(ctx, data);
    return result;
}

int GuestZlib::Reset(wa_host_call_ctx_t& ctx, Operation op) {
    const auto it = streams_.find(ctx.args[0]);
    const bool encode = op == Operation::DeflateReset;
    if (it == streams_.end() || it->second.encode != encode) return Z_STREAM_ERROR;
    auto& data = it->second.data;
    int result;
    if (encode) result = deflateReset(&data);
    else if (op == Operation::InflateResetWindow) result = inflateReset2(&data, static_cast<int32_t>(ctx.args[1]));
    else result = inflateReset(&data);
    if (result == Z_OK) Publish(ctx, data);
    return result;
}

int GuestZlib::End(wa_host_call_ctx_t& ctx, bool encode) {
    const auto it = streams_.find(ctx.args[0]);
    if (it == streams_.end() || it->second.encode != encode) return Z_STREAM_ERROR;
    auto& data = it->second.data;
    const auto result = encode ? deflateEnd(&data) : inflateEnd(&data);
    const GuestMemory mem(ctx);
    mem.WriteWord(ctx.args[0] + State * mem.width(), 0);
    streams_.erase(it);
    return result;
}

void GuestZlib::Gzip(wa_host_call_ctx_t& ctx, Operation op) {
    const GuestMemory mem(ctx);
    if (op == Operation::GzipOpen) {
        const auto path = dbt::GuestPath(ctx, ctx.args[0]);
        const auto mode = mem.String(ctx.args[1], 32);
        errno = 0;
        gzFile file;
        if (const auto mounted = dbt::FindMountedFile(*ctx.dbt, path)) {
            if (!mode.starts_with('r') || mode.contains('+')) {
                dbt::SetErrno(*ctx.dbt, dbt::GuestErrno(EROFS));
                return;
            }
            const auto fd = dbt::OpenMountedFile(*mounted, O_RDONLY | O_CLOEXEC);
            file = fd >= 0 ? gzdopen(fd, mode.c_str()) : nullptr;
            if (!file && fd >= 0) {
                const int error = errno;
                close(fd);
                errno = error;
            }
        } else {
            file = gzopen(path.c_str(), mode.c_str());
        }
        if (!file) {
            if (errno) dbt::SetErrno(*ctx.dbt, dbt::GuestErrno(errno));
            return;
        }
        std::unique_ptr<gzFile_s, decltype(&gzclose)> owned(file, gzclose);
        const auto handle = dbt::AllocateGuest(*ctx.dbt, 3 * mem.width());
        if (!handle) throw std::bad_alloc{};
        try {
            gzip_files_.emplace(handle, std::move(owned));
        } catch (...) {
            dbt::FreeGuest(*ctx.dbt, handle);
            throw;
        }
        ctx.ret0 = handle;
        return;
    }

    const auto entry = gzip_files_.find(ctx.args[0]);
    if (entry == gzip_files_.end()) {
        if (op == Operation::GzipClose) ctx.ret0 = Z_STREAM_ERROR;
        if (op == Operation::GzipPuts || op == Operation::GzipRewind) ctx.ret0 = -1;
        return;
    }
    const auto file = entry->second.get();
    switch (op) {
    case Operation::GzipClose:
        ctx.ret0 = gzclose(entry->second.release());
        dbt::FreeGuest(*ctx.dbt, entry->first);
        gzip_files_.erase(entry);
        break;
    case Operation::GzipEof: ctx.ret0 = gzeof(file); break;
    case Operation::GzipGets: {
        const auto length = static_cast<int32_t>(ctx.args[2]);
        if (length <= 0 || length > kMaxBuffer) return;
        std::vector<char> line(length);
        const auto start = gztell(file);
        if (gzgets(file, line.data(), length)) {
            const auto count = gztell(file) - start;
            if (count < 0 || count >= length) return;
            mem.Write(ctx.args[1], line.data(), count + 1);
            ctx.ret0 = ctx.args[1];
        }
        break;
    }
    case Operation::GzipPuts: ctx.ret0 = gzputs(file, mem.String(ctx.args[1], kMaxBuffer).c_str()); break;
    case Operation::GzipRewind: ctx.ret0 = gzrewind(file); break;
    default: break;
    }
}

template<GuestZlib::Operation op>
int32_t GuestZlib::Dispatch(wa_host_call_ctx_t* ctx, void* user) {
    auto& bridge = *static_cast<GuestZlib*>(user);
    try {
        if constexpr (op == Operation::Compress || op == Operation::Uncompress) {
            ctx->ret0 = TransformBuffer(*ctx, op == Operation::Compress ? compress : uncompress);
        } else if constexpr (op == Operation::CompressBound) {
            // Android zlib rounds up for its alternative hash function.
            const auto length = ctx->args[0];
            const auto size = length + (length >> 12) + (length >> 14) + (length >> 25) + 13;
            const auto bound = size + (size >> 7);
            const uint64_t limit = ctx->arch == WA_ARCH_ARM32 ? UINT32_MAX : UINT64_MAX;
            ctx->ret0 = size < length || bound < size || bound > limit ? limit : bound;
        } else if constexpr (op == Operation::CompileFlags) {
            ctx->ret0 = CompileFlags(*ctx);
        } else if constexpr (op == Operation::CrcTable) {
            if (!bridge.crc_table_) {
                constexpr size_t bytes = 256 * sizeof(z_crc_t);
                dbt::GuestBuffer table(*ctx->dbt, bytes);
                GuestMemory(*ctx).Write(table.address, get_crc_table(), bytes);
                bridge.crc_table_ = std::exchange(table.address, 0);
            }
            ctx->ret0 = bridge.crc_table_;
        } else if constexpr (op == Operation::DeflateBound) {
            const auto it = bridge.streams_.find(ctx->args[0]);
            auto* stream = it != bridge.streams_.end() && it->second.encode ? &it->second.data : nullptr;
            ctx->ret0 = deflateBound_z(stream, ctx->args[1]);
        } else if constexpr (op >= Operation::GzipOpen) {
            bridge.Gzip(*ctx, op);
        } else if constexpr (op == Operation::DeflateInit || op == Operation::InflateInit ||
                      op == Operation::DeflateInitDefault || op == Operation::InflateInitDefault) {
            constexpr bool encode = op == Operation::DeflateInit || op == Operation::DeflateInitDefault;
            constexpr bool default_window = op == Operation::DeflateInitDefault || op == Operation::InflateInitDefault;
            ctx->ret0 = bridge.Initialize<encode>(*ctx, default_window);
        } else if constexpr (op == Operation::Deflate || op == Operation::Inflate) {
            ctx->ret0 = bridge.Process(*ctx, op == Operation::Deflate);
        } else if constexpr (op == Operation::DeflateReset || op == Operation::InflateReset ||
                             op == Operation::InflateResetWindow) {
            ctx->ret0 = bridge.Reset(*ctx, op);
        } else {
            ctx->ret0 = bridge.End(*ctx, op == Operation::DeflateEnd);
        }
        return WA_OK;
    } catch (const dbt::GuestFault& fault) {
        return fault.status;
    } catch (const std::bad_alloc&) {
        if constexpr (op == Operation::GzipOpen || op == Operation::GzipGets) {
            dbt::SetErrno(*ctx->dbt, dbt::GuestErrno(ENOMEM));
        } else {
            ctx->ret0 = Z_MEM_ERROR;
        }
        return WA_OK;
    }
}

void GuestZlib::Register(wa_dbt_t* dbt) {
    constexpr std::pair<const char*, wa_host_symbol_fn> functions[]{
        {"deflateInit2_", Dispatch<Operation::DeflateInit>}, {"inflateInit2_", Dispatch<Operation::InflateInit>},
        {"deflateInit_", Dispatch<Operation::DeflateInitDefault>},
        {"inflateInit_", Dispatch<Operation::InflateInitDefault>},
        {"deflate", Dispatch<Operation::Deflate>}, {"inflate", Dispatch<Operation::Inflate>},
        {"deflateReset", Dispatch<Operation::DeflateReset>}, {"inflateReset", Dispatch<Operation::InflateReset>},
        {"inflateReset2", Dispatch<Operation::InflateResetWindow>},
        {"deflateEnd", Dispatch<Operation::DeflateEnd>}, {"inflateEnd", Dispatch<Operation::InflateEnd>},
        {"compress", Dispatch<Operation::Compress>}, {"uncompress", Dispatch<Operation::Uncompress>},
        {"compressBound", Dispatch<Operation::CompressBound>},
        {"zlibCompileFlags", Dispatch<Operation::CompileFlags>},
        {"get_crc_table", Dispatch<Operation::CrcTable>}, {"deflateBound", Dispatch<Operation::DeflateBound>},
        {"gzopen", Dispatch<Operation::GzipOpen>}, {"gzclose", Dispatch<Operation::GzipClose>},
        {"gzeof", Dispatch<Operation::GzipEof>}, {"gzgets", Dispatch<Operation::GzipGets>},
        {"gzputs", Dispatch<Operation::GzipPuts>}, {"gzrewind", Dispatch<Operation::GzipRewind>},
    };
    for (const auto& [name, thunk] : functions)
        dbt::CheckGuest(wa_dbt_register_symbol(dbt, "libz.so", name, thunk, this));
}

} // namespace wa::system
