#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "dex/dex_file.h"

namespace wa::aosp17 {

// The returned DexFiles own their backing storage, including extracted ZIP entries.
std::vector<std::unique_ptr<const ::art::DexFile>> OpenDexFiles(
    std::span<const uint8_t> input, const std::string& location);

// Compile the primary logical DEX with CodeGeneratorWasm. Uses the wa_art_compile
// profile syntax. Loading, verification and code generation failures throw.
std::vector<uint8_t> CompileDexToWasm(std::span<const uint8_t> input, const char* profile = nullptr);

} // namespace wa::aosp17
