#pragma once

#include "apk_loader.h"

#include <span>
#include <string_view>
#include <vector>

namespace wa::system {

int32_t CompileApkDex(std::span<const uint8_t> bytes, std::string_view descriptor,
                      std::vector<uint8_t>& wasm, wa_system_apk_dex_stats_t& stats);

} // namespace wa::system
