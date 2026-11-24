#pragma once

#include "wasmandroid/softmmu_linker.h"

#include <map>
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

    enum class Operation { DeflateInit, InflateInit, Deflate, Inflate, DeflateEnd, InflateEnd };
    template<Operation op> static int32_t Dispatch(wa_host_call_ctx_t* ctx, void* user);
    template<bool encode> int Initialize(wa_host_call_ctx_t& ctx);
    int Process(wa_host_call_ctx_t& ctx, bool encode);
    int End(wa_host_call_ctx_t& ctx, bool encode);

    std::map<uint64_t, Stream> streams_;
};

} // namespace wa::system
