#pragma once

#include "wasmandroid/softmmu_linker.h"

#include <map>
#include <string>
#include <variant>
#include <vector>

namespace wa::system {

class GuestMedia {
  public:
    void Register(wa_dbt_t* dbt);

  private:
    enum class FormatOp { New, Delete, GetInt, GetString, SetInt, SetFloat, SetString, SetBuffer, String };
    using Value = std::variant<int32_t, float, std::string, std::vector<uint8_t>>;
    struct Format {
        std::map<std::string, Value, std::less<>> values;
        uint64_t text = 0;
    };
    template<FormatOp operation> static int32_t FormatCall(wa_host_call_ctx_t* ctx, void* user);
    template<FormatOp operation> void DispatchFormat(wa_host_call_ctx_t& ctx);

    std::map<uint64_t, Format> formats_;
};

} // namespace wa::system
