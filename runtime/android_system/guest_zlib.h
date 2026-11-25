#pragma once

#include "wasmandroid/softmmu_linker.h"

#include <map>
#include <memory>
#include <zlib.h>

namespace wa::system {

class GuestZlib {
  public:
    void Register(wa_dbt_t* dbt);

  private:
    struct Stream {
        explicit Stream(bool encode) : encode(encode) {}
        ~Stream();
        bool encode;
        z_stream data{};
    };

    enum class Operation {
        DeflateInit, DeflateInitDefault, InflateInit, InflateInitDefault, Deflate, Inflate,
        DeflateReset, InflateReset, InflateResetWindow, DeflateEnd, InflateEnd,
        Compress, CompressBound, Uncompress, CompileFlags, CrcTable, DeflateBound,
        GzipOpen, GzipClose, GzipEof, GzipGets, GzipPuts, GzipRewind
    };
    template<Operation op> static int32_t Dispatch(wa_host_call_ctx_t* ctx, void* user);
    template<bool encode> int Initialize(wa_host_call_ctx_t& ctx, bool default_window);
    int Process(wa_host_call_ctx_t& ctx, bool encode);
    int Reset(wa_host_call_ctx_t& ctx, Operation op);
    int End(wa_host_call_ctx_t& ctx, bool encode);
    void Gzip(wa_host_call_ctx_t& ctx, Operation op);

    std::map<uint64_t, Stream> streams_;
    std::map<uint64_t, std::unique_ptr<gzFile_s, decltype(&gzclose)>> gzip_files_;
    uint64_t crc_table_ = 0;
};

} // namespace wa::system
