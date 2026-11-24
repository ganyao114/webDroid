#include "guest_zlib.h"

#include "bionic_internal.h"

namespace wa::system {
namespace {

using dbt::CallReader;
using dbt::GuestMemory;

// Android uses an ILP32 or LP64 z_stream. Never expose the host z_stream or native pointers.
enum Slot { NextIn, AvailableIn, TotalIn, NextOut, AvailableOut, TotalOut, Message, State,
            Allocate, Release, Opaque, DataType, Adler, Reserved, Count };

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

} // namespace

GuestZlib::Stream::~Stream() {
    if (data.state) {
        if (encode) deflateEnd(&data);
        else inflateEnd(&data);
    }
}

template<bool encode> int GuestZlib::Initialize(wa_host_call_ctx_t& ctx) {
    const GuestMemory mem(ctx);
    CallReader args(ctx);
    const auto base = args.Next();
    int level = 0, method = 0, memory_level = 0, strategy = 0;
    if constexpr (encode) {
        level = static_cast<int32_t>(args.Next());
        method = static_cast<int32_t>(args.Next());
    }
    const auto bits = static_cast<int32_t>(args.Next());
    if constexpr (encode) {
        memory_level = static_cast<int32_t>(args.Next());
        strategy = static_cast<int32_t>(args.Next());
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
        const int result = encode ? deflateInit2(&data, level, method, bits, memory_level, strategy)
                                  : inflateInit2(&data, bits);
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
    constexpr uint32_t max_buffer = 64 * 1024 * 1024;
    if (available_in > max_buffer || available_out > max_buffer) return Z_MEM_ERROR;
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

template<GuestZlib::Operation op>
int32_t GuestZlib::Dispatch(wa_host_call_ctx_t* ctx, void* user) {
    auto& bridge = *static_cast<GuestZlib*>(user);
    try {
        if constexpr (op == Operation::DeflateInit || op == Operation::InflateInit) {
            ctx->ret0 = bridge.Initialize<op == Operation::DeflateInit>(*ctx);
        } else if constexpr (op == Operation::Deflate || op == Operation::Inflate) {
            ctx->ret0 = bridge.Process(*ctx, op == Operation::Deflate);
        } else {
            ctx->ret0 = bridge.End(*ctx, op == Operation::DeflateEnd);
        }
        return WA_OK;
    } catch (const dbt::GuestFault& fault) {
        return fault.status;
    } catch (const std::bad_alloc&) {
        ctx->ret0 = Z_MEM_ERROR;
        return WA_OK;
    }
}

void GuestZlib::Register(wa_dbt_t* dbt) {
    constexpr std::pair<const char*, wa_host_symbol_fn> functions[]{
        {"deflateInit2_", Dispatch<Operation::DeflateInit>}, {"inflateInit2_", Dispatch<Operation::InflateInit>},
        {"deflate", Dispatch<Operation::Deflate>}, {"inflate", Dispatch<Operation::Inflate>},
        {"deflateEnd", Dispatch<Operation::DeflateEnd>}, {"inflateEnd", Dispatch<Operation::InflateEnd>},
    };
    for (const auto& [name, thunk] : functions)
        dbt::CheckGuest(wa_dbt_register_symbol(dbt, "libz.so", name, thunk, this));
}

} // namespace wa::system
